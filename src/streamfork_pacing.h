/**
 * @file src/streamfork_pacing.h
 * @brief Optional video packet pacing for lossy links such as Wi-Fi (streamfork, option packet_pacing_mbps).
 *
 * Apollo sends the packets of each video frame at ~80% of 1 Gbps. With packet_pacing_mbps > 0 the frame is
 * spread at the configured rate instead, but never slower than what is needed to send the whole frame within
 * FRAME_BUDGET_PERCENT of the frame interval, and never faster than the original rate.
 *
 * Only depends on the standard library so it can be tested on its own (see streamfork_pacing_selftest.cpp).
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace streamfork::pacing {
  /// A frame must be sent within this share of the frame interval (measured from the moment it is ready to be sent)
  constexpr int FRAME_BUDGET_PERCENT = 50;

  /// The rate control numbers used by videoBroadcastThread() for one frame.
  struct rate_t {
    std::size_t packets_per_group;  ///< packets allowed per pacing group (ratecontrol_packets_in_1ms)
    std::chrono::nanoseconds group_duration;  ///< duration of a pacing group (1 ms originally)
    std::size_t send_batch_size;  ///< packets per send_batch() call
  };

  struct frame_t {
    int configured_mbps;  ///< packet_pacing_mbps, 0 = off
    std::size_t blocksize;  ///< bytes per shard as counted by the original rate control (packetsize + RTP header)
    std::size_t frame_packets;  ///< data and parity shards of all FEC blocks of the frame
    int framerate;  ///< framerate requested by the client
    std::chrono::nanoseconds backlog;  ///< how long the frame waits for the previous one before it can start
  };

  struct result_t {
    rate_t rate;
    std::uint64_t rate_bps;  ///< effective rate of this frame in bits/s (blocksize accounting)
    bool floor_raised;  ///< the configured rate was too slow for this frame and was raised
    bool original;  ///< the original rate control numbers are used unchanged
  };

  /// Original Apollo numbers: around 80% of 1 Gbps in 1 ms groups, batches below 64 KiB and at most 64 packets.
  rate_t original_rate(std::size_t blocksize);

  /// Data plus parity shards that fec::encode() produces for one FEC block of block_bytes bytes.
  std::size_t block_packet_count(std::size_t block_bytes, std::size_t blocksize, int fec_percentage, int min_parity_shards);

  /// Time budget of a frame: FRAME_BUDGET_PERCENT of the interval at this framerate (60 fps if unknown).
  std::chrono::nanoseconds frame_budget(int framerate);

  /**
   * @brief Rate control numbers for one frame.
   * @param frame The frame and the configuration.
   * @param original The numbers the original code computed (returned unchanged when pacing is off or not slower).
   */
  result_t compute(const frame_t &frame, const rate_t &original);

  /// Text for the once-per-session info log line.
  std::string describe(int configured_mbps, std::size_t blocksize, int framerate);
}  // namespace streamfork::pacing
