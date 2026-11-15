/**
 * @file src/streamfork_pacing_selftest.cpp
 * @brief Standalone self-test of streamfork_pacing.cpp (not part of the sunshine target).
 *
 * Build and run from the repository root:
 *   g++ -std=c++20 -Wall -Wextra -O2 -o pacing_selftest src/streamfork_pacing_selftest.cpp src/streamfork_pacing.cpp
 *   ./pacing_selftest
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ratio>
#include <vector>

// local includes
#include "streamfork_pacing.h"

using namespace std::chrono_literals;
namespace pacing = streamfork::pacing;

namespace {
  int failures = 0;
  int checks = 0;

  void check(bool ok, const char *what, int line) {
    ++checks;
    if (!ok) {
      ++failures;
      std::printf("FAIL line %d: %s\n", line, what);
    }
  }

#define CHECK(x) check((x), #x, __LINE__)

  constexpr int DATA_SHARDS_MAX = 255;
  constexpr int MAX_FEC_BLOCKS = 4;
  constexpr std::size_t RTP_HEADER = 16;  // MAX_RTP_HEADER_SIZE
  constexpr std::size_t VIDEO_PACKET_RAW = 32;  // sizeof(video_packet_raw_t): RTP + NV_VIDEO_PACKET headers

  /// FEC block sizes as videoBroadcastThread() splits a frame of encoded_bytes (headers inserted per shard)
  std::vector<std::size_t> split_frame(std::size_t encoded_bytes, std::size_t blocksize, int &fec_percentage) {
    const auto payload_blocksize = blocksize - VIDEO_PACKET_RAW;
    const auto with_header = encoded_bytes + 8;  // video_short_frame_header_t
    const auto shards = (with_header + payload_blocksize - 1) / payload_blocksize;
    const auto payload = shards * blocksize - (shards * payload_blocksize - with_header);

    const auto max_data_per_block = (std::size_t) ((DATA_SHARDS_MAX * 100) / (100 + fec_percentage)) * blocksize;
    auto blocks = (payload + max_data_per_block - 1) / max_data_per_block;
    if (blocks > MAX_FEC_BLOCKS) {
      fec_percentage = 0;
      blocks = MAX_FEC_BLOCKS;
    }
    const auto aligned = ((payload / blocks + blocksize - 1) / blocksize) * blocksize;
    std::vector<std::size_t> sizes;
    for (std::size_t x = 0; x < blocks; ++x) {
      sizes.push_back(x == blocks - 1 ? payload - x * aligned : aligned);
    }
    return sizes;
  }

  struct sim_t {
    std::chrono::nanoseconds send_time;  ///< from frame start to ratecontrol_next_frame_start
    std::size_t max_burst;  ///< most packets sent without a wait in between
    std::size_t sleeps;  ///< timer->sleep_for() calls
    std::size_t batches;  ///< send_batch() calls
    std::size_t packets;
  };

  /// Replays the send loop of videoBroadcastThread() with instantaneous sends
  sim_t simulate(const std::vector<std::size_t> &block_packets, const pacing::rate_t &rate) {
    sim_t sim {0ns, 0, 0, 0, 0};
    std::chrono::nanoseconds now = 0ns;
    std::size_t frame_sent = 0, group_sent = 0, burst = 0;

    for (auto shards : block_packets) {
      std::size_t next = 0;
      for (std::size_t x = 0; x < shards; ++x) {
        if (x - next + 1 >= rate.send_batch_size || x + 1 == shards) {
          if (group_sent >= rate.packets_per_group || frame_sent == 0) {
            auto due = rate.group_duration * frame_sent / rate.packets_per_group;
            if (now < due) {
              now = due;
              ++sim.sleeps;
              burst = 0;
            }
            group_sent = 0;
          }
          auto batch = x - next + 1;
          ++sim.batches;
          burst += batch;
          sim.max_burst = std::max(sim.max_burst, burst);
          group_sent += batch;
          frame_sent += batch;
          next = x + 1;
        }
      }
      sim.send_time = rate.group_duration * frame_sent / rate.packets_per_group;
    }
    sim.packets = frame_sent;
    return sim;
  }

  struct frame_case_t {
    std::vector<std::size_t> block_packets;
    std::size_t total;
  };

  frame_case_t make_frame(std::size_t encoded_bytes, std::size_t blocksize, int fec_percentage = 20, int min_parity = 2) {
    frame_case_t f;
    f.total = 0;
    for (auto bytes : split_frame(encoded_bytes, blocksize, fec_percentage)) {
      auto n = pacing::block_packet_count(bytes, blocksize, fec_percentage, min_parity);
      f.block_packets.push_back(n);
      f.total += n;
    }
    return f;
  }

  bool same(const pacing::rate_t &a, const pacing::rate_t &b) {
    return a.packets_per_group == b.packets_per_group && a.group_duration == b.group_duration && a.send_batch_size == b.send_batch_size;
  }

  /// Invariants that must hold for every input
  void check_invariants(const pacing::frame_t &in, const pacing::result_t &r, const pacing::rate_t &orig, const frame_case_t &f) {
    CHECK(r.rate.packets_per_group >= 1);
    CHECK(r.rate.send_batch_size >= 1);
    CHECK(r.rate.send_batch_size <= std::max<std::size_t>(1, orig.send_batch_size));
    CHECK(r.rate.packets_per_group <= orig.packets_per_group);
    CHECK(r.rate.group_duration >= 1ms - 1us);  // groups never shorter than the original 1 ms
    if (r.original) {
      CHECK(same(r.rate, orig));
    } else {
      // Paced: one send_batch() per group, never faster than the original rate, never slower than configured
      CHECK(r.rate.send_batch_size == std::min(orig.send_batch_size, r.rate.packets_per_group));
      CHECK(r.rate_bps < (std::uint64_t) orig.packets_per_group * in.blocksize * 8 * 1000);
      CHECK(r.rate_bps >= (std::uint64_t) in.configured_mbps * 1'000'000);
      // Floor: the whole frame leaves within the budget (minus the backlog)
      auto sim = simulate(f.block_packets, r.rate);
      CHECK(sim.send_time <= pacing::frame_budget(in.framerate) - in.backlog);
      CHECK(sim.packets == f.total);
      // Pacing groups really are small: never more than two groups' worth in one go (FEC block boundaries)
      CHECK(sim.max_burst <= 2 * r.rate.packets_per_group);
    }
  }

  void report(const char *name, std::size_t encoded_bytes, int fps, int mbps, std::size_t blocksize = 1408, int fec = 20) {
    auto orig = pacing::original_rate(blocksize);
    auto f = make_frame(encoded_bytes, blocksize, fec);
    pacing::frame_t in {mbps, blocksize, f.total, fps, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    auto sim = simulate(f.block_packets, r.rate);
    auto base = simulate(f.block_packets, orig);
    std::printf("%-26s %4d fps %5d Mbps cfg | %4zu pkts | %7.1f Mbps eff%s%s | send %6.2f ms (orig %5.2f) | group %2zu pkts/%.3f ms, batch %2zu, %3zu sleeps, max burst %3zu\n",
                name, fps, mbps, f.total, r.rate_bps / 1e6, r.floor_raised ? " FLOOR" : "      ", r.original ? " ORIG" : "     ",
                sim.send_time.count() / 1e6, base.send_time.count() / 1e6, r.rate.packets_per_group, r.rate.group_duration.count() / 1e6,
                r.rate.send_batch_size, sim.sleeps, sim.max_burst);
  }
}  // namespace

int main() {
  // --- original numbers (the expressions copied from videoBroadcastThread) ---
  for (std::size_t blocksize : {1040ul, 1408ul, 1017ul, 577ul, 256ul, 9016ul, 1500ul}) {
    auto o = pacing::original_rate(blocksize);
    CHECK(o.packets_per_group == (std::size_t) (std::giga::num * 80 / 100 / 1000 / (int) blocksize / 8));
    CHECK(o.group_duration == std::chrono::duration_cast<std::chrono::nanoseconds>(1ms));
    CHECK(o.send_batch_size == std::min<std::size_t>(64, 64 * 1024 / (int) blocksize));
  }
  CHECK(pacing::original_rate(1408).packets_per_group == 71);
  CHECK(pacing::original_rate(1408).send_batch_size == 46);
  CHECK(pacing::original_rate(577).send_batch_size == 64);

  // --- option off: always the original numbers ---
  for (std::size_t blocksize : {1040ul, 1408ul, 1017ul}) {
    auto orig = pacing::original_rate(blocksize);
    for (std::size_t packets : {1ul, 144ul, 5000ul}) {
      for (int fps : {0, 30, 60, 120, 240}) {
        for (int mbps : {0, -5}) {
          auto r = pacing::compute({mbps, blocksize, packets, fps, 3ms}, orig);
          CHECK(r.original);
          CHECK(!r.floor_raised);
          CHECK(same(r.rate, orig));
        }
      }
    }
  }

  // --- fec::encode() packet count ---
  CHECK(pacing::block_packet_count(1408 * 100, 1408, 20, 2) == 120);
  CHECK(pacing::block_packet_count(1408 * 100 + 1, 1408, 20, 2) == 101 + 21);
  CHECK(pacing::block_packet_count(1408 * 3, 1408, 20, 2) == 3 + 2);  // minimum parity shards
  CHECK(pacing::block_packet_count(1408 * 3, 1408, 0, 2) == 3);  // FEC disabled for this frame
  CHECK(pacing::block_packet_count(1, 1408, 20, 0) == 2);

  // --- frame budget ---
  CHECK(pacing::frame_budget(60) == 8'333'333ns);
  CHECK(pacing::frame_budget(0) == pacing::frame_budget(60));
  CHECK(pacing::frame_budget(120) == 4'166'666ns);

  const std::size_t bs = 1408;  // packetsize 1392 + RTP header
  const auto orig = pacing::original_rate(bs);

  // --- typical 4K60 80 Mbps P-frame (~167 KB): 100 Mbps would take ~16 ms, the floor raises it ---
  {
    auto f = make_frame(80'000'000 / 8 / 60, bs);
    pacing::frame_t in {100, bs, f.total, 60, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(!r.original);
    CHECK(r.floor_raised);
    CHECK(r.rate_bps > 150'000'000 && r.rate_bps < 250'000'000);
    // 400 Mbps fits: no floor, paced at ~400
    in.configured_mbps = 400;
    r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(!r.original && !r.floor_raised);
    CHECK(r.rate.packets_per_group == 36);  // ceil(400e6 * 1ms / 11264)
  }

  // --- huge IDR (1 MB): even the original rate is slower than the floor, so the original numbers are used ---
  {
    auto f = make_frame(1'000'000, bs);
    pacing::frame_t in {100, bs, f.total, 60, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(r.original && r.floor_raised);
    CHECK(same(r.rate, orig));
  }

  // --- medium IDR (300 KB) at 100 Mbps: raised by the floor but still below the original rate ---
  {
    auto f = make_frame(300'000, bs);
    pacing::frame_t in {100, bs, f.total, 60, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(!r.original && r.floor_raised);
  }

  // --- tiny frame (1 packet) at a very low rate: paced, and still within the budget ---
  {
    auto f = make_frame(100, bs);
    CHECK(f.total == 1 + 2);  // 1 data shard + minimum parity
    for (int mbps : {1, 5, 10}) {
      pacing::frame_t in {mbps, bs, f.total, 60, 0ns};
      auto r = pacing::compute(in, orig);
      check_invariants(in, r, orig, f);
      CHECK(!r.original);
      CHECK(r.rate.packets_per_group == 1 && r.rate.send_batch_size == 1);
    }
  }

  // --- backlog: the previous frame is still being sent ---
  {
    auto f = make_frame(30'000'000 / 8 / 60, bs);
    pacing::frame_t in {100, bs, f.total, 60, 0ns};
    auto free_run = pacing::compute(in, orig);
    in.backlog = 4ms;
    auto late = pacing::compute(in, orig);
    check_invariants(in, late, orig, f);
    CHECK(!free_run.floor_raised);
    CHECK(late.floor_raised && late.rate_bps > free_run.rate_bps);
    in.backlog = 9ms;  // more than the whole budget: as fast as the original
    auto very_late = pacing::compute(in, orig);
    CHECK(very_late.original && same(very_late.rate, orig));
  }

  // --- very high configured rates: at or above the original rate means the original numbers ---
  // (the original rate at blocksize 1408 is 71 packets/ms = 799.7 Mbps)
  for (int mbps : {800, 1000, 10000}) {
    auto f = make_frame(150'000, bs);
    pacing::frame_t in {mbps, bs, f.total, 60, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(r.original);
  }
  {
    // Just below the original rate: paced, numbers close to the original ones
    auto f = make_frame(150'000, bs);
    pacing::frame_t in {790, bs, f.total, 60, 0ns};
    auto r = pacing::compute(in, orig);
    check_invariants(in, r, orig, f);
    CHECK(!r.original && r.rate.packets_per_group == 71 && r.rate.send_batch_size == 46);
  }

  // --- odd packet sizes, framerates and rates: invariants only ---
  for (std::size_t blocksize : {256ul, 577ul, 1017ul, 1040ul, 1408ul, 1500ul, 9016ul}) {
    auto o = pacing::original_rate(blocksize);
    for (std::size_t bytes : {50ul, 4'000ul, 60'000ul, 170'000ul, 400'000ul, 1'200'000ul}) {
      auto f = make_frame(bytes, blocksize);
      for (int fps : {0, 24, 30, 60, 90, 120, 144, 240, 1000}) {
        for (int mbps : {1, 3, 20, 50, 100, 150, 200, 333, 400, 600, 799, 5000}) {
          for (std::chrono::nanoseconds backlog : {0ns, 500'000ns, 2'000'000ns}) {
            pacing::frame_t in {mbps, blocksize, f.total, fps, backlog};
            check_invariants(in, pacing::compute(in, o), o, f);
          }
        }
      }
    }
  }

  // --- describe() ---
  std::printf("%s\n", pacing::describe(200, bs, 60).c_str());
  std::printf("%s\n", pacing::describe(1000, bs, 60).c_str());

  // --- numbers for the report (blocksize 1408: Moonlight packet size 1392) ---
  std::printf("\n");
  for (int mbps : {0, 100, 200, 400}) {
    report("4K60 80 Mbps P-frame", 80'000'000 / 8 / 60, 60, mbps);
    report("4K60 30 Mbps P-frame", 30'000'000 / 8 / 60, 60, mbps);
    report("IDR 1 MB", 1'000'000, 60, mbps);
    report("IDR 300 KB", 300'000, 60, mbps);
    report("Tiny 2 KB", 2'000, 60, mbps);
  }

  std::printf("\n%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
