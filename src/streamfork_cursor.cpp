/**
 * @file src/streamfork_cursor.cpp
 * @brief Host state of the streamfork local cursor, shared by the capture and control stream threads.
 */
// standard includes
#include <atomic>
#include <mutex>

// local includes
#include "config.h"
#include "logging.h"
#include "streamfork_cursor.h"

using namespace std::literals;

namespace streamfork::cursor {
  namespace {
    std::mutex mutex;
    shape_cache_t shape_cache;
    host_cursor_t current;  ///< shape_id is 0 while no shape is known or it can't be drawn by clients
    int sources = 0;  ///< Capture backends that publish the cursor shape

    std::atomic<bool> all_sessions_local {false};
    std::atomic<bool> shape_available {false};
  }  // namespace

  bool supported() {
#ifdef _WIN32
    // Only Desktop Duplication provides the pointer shape: no local cursor with WGC
    return config::stream.local_cursor && config::video.capture != "wgc";
#else
    return false;
#endif
  }

  std::uint32_t negotiate(session_state_t &state, std::uint32_t requested) {
    state = {};
    state.granted = (requested & CAP_LOCAL_CURSOR) && supported();

    return state.granted ? CAP_LOCAL_CURSOR : 0;
  }

  void set_active(session_state_t &state, bool active) {
    state.active = active && state.granted;

    // Report the current state again: the client ignored our updates while it was inactive
    state.state_sent = false;
  }

  void resync(session_state_t &state) {
    state.state_sent = false;
    state.client_shapes.clear();
  }

  std::vector<message_t> pending_for(session_state_t &state) {
    if (!state.granted || !state.active) {
      return {};
    }

    host_cursor_t host;
    {
      std::lock_guard lg {mutex};
      if (sources > 0 && all_sessions_local) {
        host = current;
      } else {
        // The cursor is in the video: no Desktop Duplication capture (e.g. WGC fallback),
        // or another session doesn't draw it locally. Shape 0 avoids a double cursor.
        host.visible = current.visible;
      }
    }

    return session_updates(state, host);
  }

  void set_all_sessions_local(bool all_local) {
    if (all_sessions_local.exchange(all_local) != all_local) {
      BOOST_LOG(info) << "streamfork local cursor: "sv << (all_local ? "clients draw the cursor, removing it from the video"sv : "drawing the cursor in the video"sv);
    }
  }

  void publish_shape(std::uint32_t type, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, std::int32_t hot_x, std::int32_t hot_y, const std::uint8_t *data, std::size_t size) {
    if (!config::stream.local_cursor) {
      return;
    }

    auto image = convert_shape(type, width, height, pitch, hot_x, hot_y, data, size);

    std::lock_guard lg {mutex};
    if (image) {
      auto image_width = image->width;
      auto image_height = image->height;

      current.shape_id = shape_cache.insert(std::move(*image));
      current.image = shape_cache.find(current.shape_id);
      shape_available = true;

      BOOST_LOG(debug) << "streamfork cursor shape "sv << current.shape_id << " (type "sv << type << ", "sv << image_width << 'x' << image_height << ')';
    } else {
      current.shape_id = 0;
      current.image.reset();
      shape_available = false;

      BOOST_LOG(debug) << "streamfork cursor shape can't be drawn by clients (type "sv << type << ", "sv << width << 'x' << height << "), drawing it in the video"sv;
    }
  }

  void publish_visibility(bool visible) {
    std::lock_guard lg {mutex};
    current.visible = visible;
  }

  bool draw_in_video() {
    return !(all_sessions_local.load(std::memory_order_relaxed) && shape_available.load(std::memory_order_relaxed));
  }

  source_t::source_t() {
    std::lock_guard lg {mutex};
    if (sources++ == 0) {
      // Desktop Duplication reports the pointer shape again when capture starts. Until then,
      // don't hand out a shape that may have changed while nobody was capturing.
      current = {};
      shape_available = false;
    }
  }

  source_t::~source_t() {
    std::lock_guard lg {mutex};
    sources--;
  }
}  // namespace streamfork::cursor
