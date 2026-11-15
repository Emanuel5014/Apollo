/**
 * @file src/streamfork_pacing.cpp
 * @brief Pure helpers of the optional video packet pacing (streamfork, option packet_pacing_mbps).
 *
 * This file only depends on the standard library so it can be tested on its own.
 */
// standard includes
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ratio>

// local includes
#include "streamfork_pacing.h"

namespace streamfork::pacing {
  namespace {
    constexpr std::uint64_t NS_PER_S = 1'000'000'000;
    constexpr std::uint64_t NS_PER_MS = 1'000'000;

    std::uint64_t bits_per_packet(std::size_t blocksize) {
      return (std::uint64_t) blocksize * 8;
    }

    /// Bits per second of the original rate control (it rounds the packets per ms down)
    std::uint64_t original_bps(const rate_t &original, std::size_t blocksize) {
      return (std::uint64_t) original.packets_per_group * bits_per_packet(blocksize) * NS_PER_S / (std::uint64_t) original.group_duration.count();
    }

    /// 1 ms groups as the original code: the high resolution timer reliably wakes up at this granularity
    rate_t rate_for(std::uint64_t bps, std::size_t blocksize, const rate_t &original) {
      const auto bits = bits_per_packet(blocksize);
      const auto bits_per_group = (long double) bps * NS_PER_MS / NS_PER_S;

      rate_t rate;
      rate.packets_per_group = std::max<std::size_t>(1, (std::size_t) std::ceil(bits_per_group / bits));
      rate.group_duration = std::chrono::nanoseconds {(std::int64_t) ((long double) rate.packets_per_group * bits * NS_PER_S / bps)};
      // One send_batch() per group, so the group really leaves in one go and then waits for the next one
      rate.send_batch_size = std::max<std::size_t>(1, std::min(original.send_batch_size, rate.packets_per_group));
      return rate;
    }
  }  // namespace

  rate_t original_rate(std::size_t blocksize) {
    rate_t rate;
    // Same expressions as videoBroadcastThread()
    rate.packets_per_group = std::giga::num * 80 / 100 / 1000 / blocksize / 8;
    rate.group_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds {1});
    rate.send_batch_size = std::min<std::size_t>(64, 64 * 1024 / blocksize);
    return rate;
  }

  std::size_t block_packet_count(std::size_t block_bytes, std::size_t blocksize, int fec_percentage, int min_parity_shards) {
    // Same as fec::encode()
    const std::size_t data_shards = (block_bytes + blocksize - 1) / blocksize;
    std::size_t parity_shards = (data_shards * (std::size_t) std::max(fec_percentage, 0) + 99) / 100;
    if (fec_percentage != 0 && min_parity_shards > 0 && parity_shards < (std::size_t) min_parity_shards) {
      parity_shards = (std::size_t) min_parity_shards;
    }
    return data_shards + parity_shards;
  }

  std::chrono::nanoseconds frame_budget(int framerate) {
    const std::int64_t fps = framerate > 0 ? framerate : 60;
    return std::chrono::nanoseconds {(std::int64_t) NS_PER_S / fps * FRAME_BUDGET_PERCENT / 100};
  }

  result_t compute(const frame_t &frame, const rate_t &original) {
    result_t result {original, 0, false, true};

    if (frame.configured_mbps <= 0 || frame.blocksize == 0 || original.packets_per_group == 0 || original.group_duration.count() <= 0) {
      return result;
    }

    const auto cap_bps = original_bps(original, frame.blocksize);
    result.rate_bps = cap_bps;

    const auto configured_bps = (std::uint64_t) frame.configured_mbps * 1'000'000;
    auto bps = configured_bps;

    // Floor: the frame must be out before FRAME_BUDGET_PERCENT of the interval has passed since it was ready,
    // including the time it waits for the end of the previous frame. Otherwise frames would queue up.
    const auto budget = frame_budget(frame.framerate) - std::max(frame.backlog, std::chrono::nanoseconds::zero());
    if (budget.count() <= 0) {
      bps = cap_bps;
    } else {
      const auto frame_bits = (long double) frame.frame_packets * bits_per_packet(frame.blocksize);
      const auto needed_bps = std::ceil(frame_bits * NS_PER_S / budget.count());
      if (needed_bps > (long double) bps) {
        bps = needed_bps >= (long double) cap_bps ? cap_bps : (std::uint64_t) needed_bps;
      }
    }
    result.floor_raised = bps > configured_bps;

    if (bps >= cap_bps) {
      // Upper bound: the original behaviour
      return result;
    }

    result.rate = rate_for(bps, frame.blocksize, original);
    result.rate_bps = bps;
    result.original = false;
    return result;
  }

  std::string describe(int configured_mbps, std::size_t blocksize, int framerate) {
    const auto original = original_rate(blocksize);
    const auto cap_bps = original_bps(original, blocksize);
    const auto configured_bps = (std::uint64_t) configured_mbps * 1'000'000;
    const auto budget = frame_budget(framerate);
    char text[512];

    if (configured_bps >= cap_bps) {
      std::snprintf(text, sizeof(text), "Packet pacing: %d Mbps is not below the original rate (%.0f Mbps), using the original pacing", configured_mbps, cap_bps / 1e6);
      return text;
    }

    const auto rate = rate_for(configured_bps, blocksize, original);
    // Largest frame that still fits in the budget at the configured rate
    const auto fit_packets = (std::uint64_t) ((long double) configured_bps * budget.count() / NS_PER_S / bits_per_packet(blocksize));
    std::snprintf(text, sizeof(text), "Packet pacing: %d Mbps, %zu packets of %zu bytes every %.3f ms (%zu per send_batch); a frame is always sent within %.1f ms (%d%% of the %d fps interval): frames above %llu packets (~%llu KB with FEC) are sent faster, at most at the original %.0f Mbps", configured_mbps, rate.packets_per_group, blocksize, rate.group_duration.count() / 1e6, rate.send_batch_size, budget.count() / 1e6, FRAME_BUDGET_PERCENT, framerate > 0 ? framerate : 60, (unsigned long long) fit_packets, (unsigned long long) (fit_packets * blocksize / 1000), cap_bps / 1e6);
    return text;
  }
}  // namespace streamfork::pacing
