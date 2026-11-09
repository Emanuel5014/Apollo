/**
 * @file src/streamfork_clipboard.cpp
 * @brief Clipboard push over the control stream (streamfork protocol extension).
 */
// standard includes
#include <cstring>
#include <functional>
#include <mutex>

// local includes
#include "logging.h"
#include "platform/common.h"
#include "streamfork_clipboard.h"
#include "utility.h"

using namespace std::literals;

namespace streamfork::clipboard {
  namespace {
    std::mutex mutex;
    std::unique_ptr<platf::deinit_t> listener;

    // Latest host clipboard change
    std::uint64_t seq = 0;
    std::string text;
    const session_state_t *origin = nullptr;  // Client whose data caused this change, so it isn't echoed back

    // Our own write to the host clipboard, recognized when the listener sees it (anti-loop)
    std::optional<std::size_t> pending_write_hash;
    const session_state_t *pending_write_origin = nullptr;

    std::string normalize_line_endings(std::string_view input) {
      std::string output;
      output.reserve(input.size());
      for (size_t i = 0; i < input.size(); i++) {
        if (input[i] == '\r' && i + 1 < input.size() && input[i + 1] == '\n') {
          continue;
        }
        output += input[i];
      }
      return output;
    }

    void on_host_clipboard_changed(std::string new_text) {
      std::lock_guard lg {mutex};

      const session_state_t *change_origin = nullptr;
      if (pending_write_hash && *pending_write_hash == std::hash<std::string> {}(new_text)) {
        change_origin = pending_write_origin;
        pending_write_hash.reset();
        pending_write_origin = nullptr;
      } else if (new_text == text) {
        // Duplicate notification for the same content
        return;
      }

      seq++;
      text = std::move(new_text);
      origin = change_origin;

      // Never log the content of the clipboard
      BOOST_LOG(debug) << "Host clipboard changed ("sv << text.size() << " bytes)"sv;
    }

    void set_host_clipboard(const session_state_t &state, std::string new_text) {
      {
        std::lock_guard lg {mutex};
        pending_write_hash = std::hash<std::string> {}(new_text);
        pending_write_origin = &state;
      }

      if (!platf::set_clipboard(new_text)) {
        BOOST_LOG(warning) << "Failed to set the host clipboard from the client"sv;

        std::lock_guard lg {mutex};
        pending_write_hash.reset();
        pending_write_origin = nullptr;
        return;
      }

      BOOST_LOG(debug) << "Host clipboard set from the client ("sv << new_text.size() << " bytes)"sv;
    }

    std::uint32_t read_u32(std::string_view payload, std::size_t offset) {
      std::uint32_t value;
      std::memcpy(&value, payload.data() + offset, sizeof(value));
      return util::endian::little(value);
    }

    void append_u32(std::string &out, std::uint32_t value) {
      value = util::endian::little(value);
      out.append((const char *) &value, sizeof(value));
    }
  }  // namespace

  void start() {
    auto new_listener = platf::watch_clipboard(on_host_clipboard_changed);

    std::lock_guard lg {mutex};
    listener = std::move(new_listener);
  }

  void stop() {
    std::unique_ptr<platf::deinit_t> old_listener;
    {
      std::lock_guard lg {mutex};
      old_listener = std::move(listener);
    }

    // Destroyed outside the lock because the listener thread may be waiting for it
    old_listener.reset();
  }

  std::uint32_t negotiate(session_state_t &state, std::uint32_t requested, bool can_read, bool can_set) {
    std::lock_guard lg {mutex};

    std::uint32_t supported = CAP_CLIPBOARD_TEXT | CAP_CLIPBOARD_TO_HOST;
    if (listener && can_read) {
      supported |= CAP_CLIPBOARD_TO_CLIENT;
    }
    if (!can_set) {
      supported &= ~CAP_CLIPBOARD_TO_HOST;
    }

    auto granted = requested & supported;
    if (!(granted & (CAP_CLIPBOARD_TO_CLIENT | CAP_CLIPBOARD_TO_HOST))) {
      granted = 0;
    }

    state.capabilities = granted;

    // Only push changes made from now on, not what's already in the clipboard
    state.sent_seq = seq;

    return granted;
  }

  void receive_chunk(session_state_t &state, std::string_view payload) {
    if (payload.size() < CLIPBOARD_HEADER_SIZE) {
      BOOST_LOG(warning) << "Discarding runt clipboard packet"sv;
      return;
    }

    auto format = (std::uint8_t) payload[0];
    auto transfer_id = read_u32(payload, 1);
    auto total_length = read_u32(payload, 5);
    auto offset = read_u32(payload, 9);
    auto chunk = payload.substr(CLIPBOARD_HEADER_SIZE);

    // A new transfer replaces any incomplete one
    if (offset == 0 || transfer_id != state.recv_transfer_id) {
      state.recv_data.reset();
      state.recv_transfer_id = transfer_id;

      if (offset != 0) {
        // We missed the start of this transfer, so ignore the rest of it
        return;
      }

      if (format != CLIPBOARD_FORMAT_TEXT || !(state.capabilities & CAP_CLIPBOARD_TEXT) || total_length > CLIPBOARD_MAX_TEXT) {
        BOOST_LOG(debug) << "Ignoring client clipboard transfer (format "sv << (int) format << ", "sv << total_length << " bytes)"sv;
        return;
      }

      state.recv_data.emplace();
      state.recv_data->reserve(total_length);
      state.recv_total = total_length;
    }

    if (!state.recv_data) {
      return;
    }

    if (offset != state.recv_data->size() || chunk.size() > state.recv_total - state.recv_data->size()) {
      BOOST_LOG(warning) << "Discarding invalid clipboard chunk (offset "sv << offset << ", "sv << chunk.size() << " bytes)"sv;
      state.recv_data.reset();
      return;
    }

    state.recv_data->append(chunk);
    if (state.recv_data->size() == state.recv_total) {
      auto new_text = normalize_line_endings(*state.recv_data);
      state.recv_data.reset();
      set_host_clipboard(state, std::move(new_text));
    }
  }

  std::optional<std::string> pending_for(session_state_t &state) {
    std::lock_guard lg {mutex};

    if (state.sent_seq == seq) {
      return std::nullopt;
    }
    state.sent_seq = seq;

    if (origin == &state || text.empty()) {
      return std::nullopt;
    }

    if (text.size() > CLIPBOARD_MAX_TEXT) {
      BOOST_LOG(debug) << "Host clipboard too large to push ("sv << text.size() << " bytes)"sv;
      return std::nullopt;
    }

    return text;
  }

  std::vector<std::string> make_chunks(session_state_t &state, std::uint8_t format, const std::string &data) {
    std::vector<std::string> chunks;
    auto transfer_id = state.send_transfer_id++;

    std::size_t offset = 0;
    do {
      auto chunk_length = std::min(data.size() - offset, CLIPBOARD_MAX_CHUNK);

      std::string chunk;
      chunk.reserve(CLIPBOARD_HEADER_SIZE + chunk_length);
      chunk += (char) format;
      append_u32(chunk, transfer_id);
      append_u32(chunk, (std::uint32_t) data.size());
      append_u32(chunk, (std::uint32_t) offset);
      chunk.append(data, offset, chunk_length);
      chunks.emplace_back(std::move(chunk));

      offset += chunk_length;
    } while (offset < data.size());

    return chunks;
  }
}  // namespace streamfork::clipboard
