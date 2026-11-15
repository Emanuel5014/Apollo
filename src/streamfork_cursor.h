/**
 * @file src/streamfork_cursor.h
 * @brief Local cursor drawn by the client with the host cursor shape (streamfork protocol extension).
 *
 * The capture thread publishes the cursor shape and visibility it gets from Desktop Duplication.
 * The control stream thread sends them to clients that negotiated CAP_LOCAL_CURSOR and told us
 * they draw the cursor themselves, and while every session does so the capture stops blending the
 * cursor into the video.
 *
 * No crypto::PERM is required: the cursor shape is already visible in the video stream.
 */
#pragma once

// standard includes
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace streamfork {
  // Control stream packet types (streamfork range 0x3100-0x31FF). Integers are little-endian.
  //   CursorShape (H->C): u32 shapeId, u16 width, u16 height, u16 hotX, u16 hotY, u32 totalLength, u32 offset, data[]
  //                       data = width * height BGRA pixels (straight alpha), split in chunks of up to 32 KiB
  //   CursorState (H->C): u8 visible, u32 shapeId (0 = the host draws the cursor in the video)
  //   CursorMode (C->H):  u8 active (the client draws the cursor with the shapes we send)
  constexpr std::uint16_t PTYPE_CURSOR_SHAPE = 0x3106;
  constexpr std::uint16_t PTYPE_CURSOR_STATE = 0x3107;
  constexpr std::uint16_t PTYPE_CURSOR_MODE = 0x3108;

  // 0x00010000 is reserved for the adaptive bitrate extension
  constexpr std::uint32_t CAP_LOCAL_CURSOR = 0x00020000;

  constexpr std::size_t CURSOR_SHAPE_HEADER_SIZE = 4 + 2 + 2 + 2 + 2 + 4 + 4;
  constexpr std::size_t CURSOR_MAX_CHUNK = 32 * 1024;
  constexpr std::uint32_t CURSOR_MAX_SIZE = 256;

  // How many shapes we assume a client still has. Clients keep at least twice as many
  // (LI_CURSOR_MIN_CACHED_SHAPES), using the same most-recently-used order.
  constexpr std::size_t CURSOR_SESSION_SHAPES = 16;

  namespace cursor {
    /**
     * @brief Pointer shape types, same values as DXGI_OUTDUPL_POINTER_SHAPE_TYPE.
     */
    enum shape_type_e : std::uint32_t {
      SHAPE_MONOCHROME = 1,  ///< AND mask followed by XOR mask, 1 bpp each (height is doubled)
      SHAPE_COLOR = 2,  ///< 32 bpp BGRA with straight alpha
      SHAPE_MASKED_COLOR = 4,  ///< 32 bpp BGR, alpha 0x00 = opaque pixel, 0xFF = XOR with the screen
    };

    /**
     * @brief A cursor image ready for the client.
     */
    struct image_t {
      std::uint16_t width = 0;
      std::uint16_t height = 0;
      std::uint16_t hot_x = 0;
      std::uint16_t hot_y = 0;
      std::vector<std::uint8_t> bgra;  ///< width * height * 4 bytes, straight alpha

      bool operator==(const image_t &other) const = default;
    };

    /**
     * @brief Convert a pointer shape to a BGRA image a client can use as a native cursor.
     *
     * Pixels that invert the screen (XOR) can't be represented with alpha, so they are drawn
     * black with a white outline (like RDP), which keeps the text I-beam visible on any background.
     * @return The image, or std::nullopt if the shape is invalid or larger than CURSOR_MAX_SIZE.
     */
    std::optional<image_t> convert_shape(std::uint32_t type, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, std::int32_t hot_x, std::int32_t hot_y, const std::uint8_t *data, std::size_t size);

    /**
     * @brief Split a cursor image into CursorShape payloads.
     */
    std::vector<std::string> make_shape_chunks(std::uint32_t shape_id, const image_t &image);

    /**
     * @brief Build a CursorState payload.
     */
    std::string make_state_payload(bool visible, std::uint32_t shape_id);

    /**
     * @brief Recently seen shapes, so a shape that comes back reuses its id (and the client's copy).
     */
    class shape_cache_t {
    public:
      /**
       * @brief Find or add a shape.
       * @return The shape id (never 0).
       */
      std::uint32_t insert(image_t image);

      std::shared_ptr<const image_t> find(std::uint32_t id) const;

    private:
      struct entry_t {
        std::uint32_t id;
        std::shared_ptr<const image_t> image;
      };

      std::deque<entry_t> entries;  ///< Most recently used first
      std::uint32_t next_id = 1;
    };

    /**
     * @brief Cursor state to report to clients.
     */
    struct host_cursor_t {
      std::uint32_t shape_id = 0;  ///< 0 when the cursor is drawn in the video
      std::shared_ptr<const image_t> image;
      bool visible = true;
    };

    /**
     * @brief A message to send on the control stream.
     */
    struct message_t {
      std::uint16_t type;
      std::string payload;
    };

    /**
     * @brief Per-session local cursor state, owned by the control stream thread.
     */
    struct session_state_t {
      bool granted = false;  ///< CAP_LOCAL_CURSOR granted in the HelloAck
      bool active = false;  ///< The client draws the cursor itself (CursorMode)

      bool state_sent = false;
      std::uint32_t sent_shape_id = 0;
      bool sent_visible = false;
      std::deque<std::uint32_t> client_shapes;  ///< Shapes the client has, most recently used first
    };

    /**
     * @brief Compute the messages that bring a client up to date with the host cursor.
     */
    std::vector<message_t> session_updates(session_state_t &state, const host_cursor_t &host);

    /**
     * @brief Check the pure functions above.
     * @return Descriptions of the failed checks (empty on success).
     */
    std::vector<std::string> self_test();

    // Host state (streamfork_cursor.cpp)

    /**
     * @brief The host can provide cursor shapes (option enabled and a capture method that supports it).
     */
    bool supported();

    /**
     * @brief Handle the capability request of a Hello.
     * @return CAP_LOCAL_CURSOR if granted, 0 otherwise.
     */
    std::uint32_t negotiate(session_state_t &state, std::uint32_t requested);

    /**
     * @brief Handle a CursorMode message.
     */
    void set_active(session_state_t &state, bool active);

    /**
     * @brief Forget what the client was sent (after a failed send), so everything is sent again.
     */
    void resync(session_state_t &state);

    /**
     * @brief Get the messages to send to a session (called by the control stream thread).
     */
    std::vector<message_t> pending_for(session_state_t &state);

    /**
     * @brief Tell the capture whether every session draws the cursor itself.
     */
    void set_all_sessions_local(bool all_local);

    /**
     * @brief Publish a new pointer shape (called by the capture thread).
     */
    void publish_shape(std::uint32_t type, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, std::int32_t hot_x, std::int32_t hot_y, const std::uint8_t *data, std::size_t size);

    /**
     * @brief Publish the pointer visibility (called by the capture thread).
     */
    void publish_visibility(bool visible);

    /**
     * @brief Whether the capture must still blend the cursor into the video.
     */
    bool draw_in_video();

    /**
     * @brief Registers a capture backend that publishes cursor shapes while it exists.
     */
    class source_t {
    public:
      source_t();
      ~source_t();
      source_t(const source_t &) = delete;
      source_t &operator=(const source_t &) = delete;
    };
  }  // namespace cursor
}  // namespace streamfork
