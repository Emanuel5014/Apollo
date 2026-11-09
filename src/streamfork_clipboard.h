/**
 * @file src/streamfork_clipboard.h
 * @brief Clipboard push over the control stream (streamfork protocol extension).
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace streamfork {
  // Control stream packet types (streamfork range 0x3100-0x31FF). Integers are little-endian.
  //   Hello (C->H):     u8 version, u32 requested capabilities
  //   HelloAck (H->C):  u8 version, u32 granted capabilities
  //   Clipboard:        u8 format, u32 transferId, u32 totalLength, u32 offset, data[]
  constexpr std::uint16_t PTYPE_CLIPBOARD_H2C = 0x3100;
  constexpr std::uint16_t PTYPE_CLIPBOARD_C2H = 0x3101;
  constexpr std::uint16_t PTYPE_HELLO = 0x3102;
  constexpr std::uint16_t PTYPE_HELLO_ACK = 0x3103;
  constexpr std::uint8_t PROTOCOL_VERSION = 1;

  constexpr std::uint32_t CAP_CLIPBOARD_TEXT = 0x00000001;
  constexpr std::uint32_t CAP_CLIPBOARD_IMAGE = 0x00000002;
  constexpr std::uint32_t CAP_CLIPBOARD_TO_CLIENT = 0x00000100;  ///< Host pushes its clipboard (needs clipboard_read)
  constexpr std::uint32_t CAP_CLIPBOARD_TO_HOST = 0x00000200;  ///< Client pushes its clipboard (needs clipboard_set)

  constexpr std::uint8_t CLIPBOARD_FORMAT_TEXT = 1;  ///< UTF-8 with LF line endings
  constexpr std::uint8_t CLIPBOARD_FORMAT_PNG = 2;

  constexpr std::size_t CLIPBOARD_HEADER_SIZE = 1 + 4 + 4 + 4;
  constexpr std::size_t CLIPBOARD_MAX_CHUNK = 32 * 1024;
  constexpr std::size_t CLIPBOARD_MAX_TEXT = 1024 * 1024;

  namespace clipboard {
    /**
     * @brief Per-session clipboard state, owned by the control stream thread.
     */
    struct session_state_t {
      std::uint32_t capabilities = 0;  ///< Granted in the HelloAck, 0 if the client never sent a Hello
      std::uint64_t sent_seq = 0;  ///< Last host clipboard change pushed to this client
      std::uint32_t send_transfer_id = 0;

      // Transfer being received from the client
      std::uint32_t recv_transfer_id = 0;
      std::uint32_t recv_total = 0;
      std::optional<std::string> recv_data;  ///< Empty when no transfer is in progress or it's ignored
    };

    /**
     * @brief Start watching the host clipboard. Called by the control stream thread.
     */
    void start();

    /**
     * @brief Stop watching the host clipboard. Called by the control stream thread.
     */
    void stop();

    /**
     * @brief Handle a Hello and compute the capabilities granted to the client.
     * @param can_read The client has the clipboard_read permission.
     * @param can_set The client has the clipboard_set permission.
     * @return The capabilities to send in the HelloAck.
     */
    std::uint32_t negotiate(session_state_t &state, std::uint32_t requested, bool can_read, bool can_set);

    /**
     * @brief Handle a clipboard chunk from the client, setting the host clipboard when complete.
     */
    void receive_chunk(session_state_t &state, std::string_view payload);

    /**
     * @brief Get the host clipboard text to push to this client, if it changed since the last call.
     */
    std::optional<std::string> pending_for(session_state_t &state);

    /**
     * @brief Split clipboard data into clipboard packet payloads.
     */
    std::vector<std::string> make_chunks(session_state_t &state, std::uint8_t format, const std::string &data);
  }  // namespace clipboard
}  // namespace streamfork
