/**
 * @file src/streamfork_cursor_image.cpp
 * @brief Pure helpers of the streamfork local cursor: shape conversion, messages and per-session updates.
 *
 * This file only depends on the standard library so it can be tested on its own.
 */
// standard includes
#include <algorithm>
#include <cstring>

// local includes
#include "streamfork_cursor.h"

namespace streamfork::cursor {
  namespace {
    constexpr std::uint32_t PIXEL_BLACK = 0xFF000000;
    constexpr std::uint32_t PIXEL_WHITE = 0xFFFFFFFF;
    constexpr std::uint32_t PIXEL_CLEAR = 0x00000000;

    // Pixels are handled as 0xAARRGGBB, which is BGRA in little-endian byte order
    std::uint32_t read_pixel(const std::uint8_t *p) {
      return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
    }

    void append_u16(std::string &out, std::uint16_t value) {
      out += (char) (value & 0xFF);
      out += (char) (value >> 8);
    }

    void append_u32(std::string &out, std::uint32_t value) {
      for (int i = 0; i < 4; i++) {
        out += (char) ((value >> (i * 8)) & 0xFF);
      }
    }

    void touch(std::deque<std::uint32_t> &shapes, std::uint32_t id) {
      if (auto it = std::find(shapes.begin(), shapes.end(), id); it != shapes.end()) {
        shapes.erase(it);
      }
      shapes.push_front(id);
      if (shapes.size() > CURSOR_SESSION_SHAPES) {
        shapes.pop_back();
      }
    }
  }  // namespace

  std::optional<image_t> convert_shape(std::uint32_t type, std::uint32_t width, std::uint32_t height, std::uint32_t pitch, std::int32_t hot_x, std::int32_t hot_y, const std::uint8_t *data, std::size_t size) {
    // Monochrome shapes contain the AND mask and then the XOR mask
    const std::uint32_t image_height = type == SHAPE_MONOCHROME ? height / 2 : height;

    if (data == nullptr || width == 0 || image_height == 0 || width > CURSOR_MAX_SIZE || image_height > CURSOR_MAX_SIZE) {
      return std::nullopt;
    }

    std::vector<std::uint32_t> pixels(width * image_height, PIXEL_CLEAR);
    std::vector<bool> invert(width * image_height, false);
    bool has_invert = false;

    switch (type) {
      case SHAPE_COLOR:
      case SHAPE_MASKED_COLOR:
        if (pitch < width * 4 || size < (std::size_t) pitch * (image_height - 1) + width * 4) {
          return std::nullopt;
        }

        for (std::uint32_t y = 0; y < image_height; y++) {
          for (std::uint32_t x = 0; x < width; x++) {
            auto pixel = read_pixel(data + (std::size_t) y * pitch + x * 4);
            auto &out = pixels[y * width + x];

            if (type == SHAPE_COLOR) {
              out = pixel;
            } else if ((pixel >> 24) == 0) {
              // Alpha 0x00: the RGB value replaces the screen
              out = pixel | 0xFF000000;
            } else if ((pixel & 0x00FFFFFF) != 0) {
              // Alpha 0xFF: XOR with the screen (XOR with black leaves the screen unchanged)
              invert[y * width + x] = true;
              has_invert = true;
            }
          }
        }
        break;

      case SHAPE_MONOCHROME:
        if (pitch < (width + 7) / 8 || size < (std::size_t) pitch * image_height * 2) {
          return std::nullopt;
        }

        for (std::uint32_t y = 0; y < image_height; y++) {
          for (std::uint32_t x = 0; x < width; x++) {
            auto bit = 0x80 >> (x % 8);
            bool and_bit = data[(std::size_t) y * pitch + x / 8] & bit;
            bool xor_bit = data[(std::size_t) (image_height + y) * pitch + x / 8] & bit;
            auto &out = pixels[y * width + x];

            if (!and_bit) {
              out = xor_bit ? PIXEL_WHITE : PIXEL_BLACK;
            } else if (xor_bit) {
              // Inverts the screen
              invert[y * width + x] = true;
              has_invert = true;
            }
          }
        }
        break;

      default:
        return std::nullopt;
    }

    // Screen inversion can't be drawn by a native cursor: draw those pixels black,
    // with a white outline on the transparent pixels around them
    if (has_invert) {
      for (std::uint32_t y = 0; y < image_height; y++) {
        for (std::uint32_t x = 0; x < width; x++) {
          if (invert[y * width + x]) {
            pixels[y * width + x] = PIXEL_BLACK;
            continue;
          }
          if ((pixels[y * width + x] >> 24) != 0) {
            continue;
          }

          bool near_invert = false;
          for (int dy = -1; dy <= 1 && !near_invert; dy++) {
            for (int dx = -1; dx <= 1 && !near_invert; dx++) {
              auto nx = (std::int64_t) x + dx;
              auto ny = (std::int64_t) y + dy;
              if (nx >= 0 && ny >= 0 && nx < width && ny < image_height) {
                near_invert = invert[ny * width + nx];
              }
            }
          }
          if (near_invert) {
            pixels[y * width + x] = PIXEL_WHITE;
          }
        }
      }
    }

    image_t image;
    image.width = (std::uint16_t) width;
    image.height = (std::uint16_t) image_height;
    image.hot_x = (std::uint16_t) std::clamp<std::int64_t>(hot_x, 0, width - 1);
    image.hot_y = (std::uint16_t) std::clamp<std::int64_t>(hot_y, 0, image_height - 1);
    image.bgra.resize(pixels.size() * 4);
    for (std::size_t i = 0; i < pixels.size(); i++) {
      image.bgra[i * 4 + 0] = (std::uint8_t) (pixels[i] & 0xFF);
      image.bgra[i * 4 + 1] = (std::uint8_t) ((pixels[i] >> 8) & 0xFF);
      image.bgra[i * 4 + 2] = (std::uint8_t) ((pixels[i] >> 16) & 0xFF);
      image.bgra[i * 4 + 3] = (std::uint8_t) (pixels[i] >> 24);
    }

    return image;
  }

  std::vector<std::string> make_shape_chunks(std::uint32_t shape_id, const image_t &image) {
    std::vector<std::string> chunks;
    const auto total = image.bgra.size();

    std::size_t offset = 0;
    do {
      auto chunk_length = std::min(total - offset, CURSOR_MAX_CHUNK);

      std::string chunk;
      chunk.reserve(CURSOR_SHAPE_HEADER_SIZE + chunk_length);
      append_u32(chunk, shape_id);
      append_u16(chunk, image.width);
      append_u16(chunk, image.height);
      append_u16(chunk, image.hot_x);
      append_u16(chunk, image.hot_y);
      append_u32(chunk, (std::uint32_t) total);
      append_u32(chunk, (std::uint32_t) offset);
      chunk.append((const char *) image.bgra.data() + offset, chunk_length);
      chunks.emplace_back(std::move(chunk));

      offset += chunk_length;
    } while (offset < total);

    return chunks;
  }

  std::string make_state_payload(bool visible, std::uint32_t shape_id) {
    std::string payload;
    payload += (char) (visible ? 1 : 0);
    append_u32(payload, shape_id);
    return payload;
  }

  std::uint32_t shape_cache_t::insert(image_t image) {
    constexpr std::size_t MAX_ENTRIES = 32;

    for (auto it = entries.begin(); it != entries.end(); ++it) {
      if (*it->image == image) {
        auto entry = *it;
        entries.erase(it);
        entries.push_front(entry);
        return entry.id;
      }
    }

    if (next_id == 0) {
      next_id = 1;
    }
    entries.push_front({next_id++, std::make_shared<const image_t>(std::move(image))});
    if (entries.size() > MAX_ENTRIES) {
      entries.pop_back();
    }

    return entries.front().id;
  }

  std::shared_ptr<const image_t> shape_cache_t::find(std::uint32_t id) const {
    for (const auto &entry : entries) {
      if (entry.id == id) {
        return entry.image;
      }
    }
    return nullptr;
  }

  std::vector<message_t> session_updates(session_state_t &state, const host_cursor_t &host) {
    std::vector<message_t> messages;

    if (!state.granted || !state.active) {
      return messages;
    }

    const auto shape_id = host.image ? host.shape_id : 0;

    // Send the shape first if the client doesn't have it. The client keeps shapes in the same
    // most-recently-used order (updated by every CursorShape and CursorState), but at least twice as many.
    if (shape_id != 0 && std::find(state.client_shapes.begin(), state.client_shapes.end(), shape_id) == state.client_shapes.end()) {
      for (auto &chunk : make_shape_chunks(shape_id, *host.image)) {
        messages.push_back({PTYPE_CURSOR_SHAPE, std::move(chunk)});
      }
      touch(state.client_shapes, shape_id);
    }

    if (!state.state_sent || state.sent_shape_id != shape_id || state.sent_visible != host.visible) {
      messages.push_back({PTYPE_CURSOR_STATE, make_state_payload(host.visible, shape_id)});
      state.state_sent = true;
      state.sent_shape_id = shape_id;
      state.sent_visible = host.visible;
      if (shape_id != 0) {
        touch(state.client_shapes, shape_id);
      }
    }

    return messages;
  }

  std::vector<std::string> self_test() {
    std::vector<std::string> failures;
    auto check = [&](bool ok, const char *what) {
      if (!ok) {
        failures.emplace_back(what);
      }
    };
    auto pixel_at = [](const image_t &image, int x, int y) {
      return read_pixel(image.bgra.data() + (y * image.width + x) * 4);
    };

    // Color: copied as is, row pitch honored, hotspot clamped
    {
      std::uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE, 9, 10, 11, 12, 13, 14, 15, 16, 0xEE, 0xEE, 0xEE, 0xEE};
      auto image = convert_shape(SHAPE_COLOR, 2, 2, 12, 5, -1, data, sizeof(data));
      check(image && image->width == 2 && image->height == 2, "color: size");
      check(image && image->hot_x == 1 && image->hot_y == 0, "color: hotspot clamp");
      check(image && image->bgra == std::vector<std::uint8_t>({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}), "color: pixels");
    }

    // Masked color: alpha 0 = opaque, XOR black = transparent, XOR white = black with white outline
    {
      // 3x1: [opaque red] [xor black] [xor white]
      std::uint32_t src[] = {0x00FF0000, 0xFF000000, 0xFFFFFFFF};
      std::uint8_t data[sizeof(src)];
      for (int i = 0; i < 3; i++) {
        for (int b = 0; b < 4; b++) {
          data[i * 4 + b] = (std::uint8_t) (src[i] >> (b * 8));
        }
      }
      auto image = convert_shape(SHAPE_MASKED_COLOR, 3, 1, 12, 0, 0, data, sizeof(data));
      check(image && pixel_at(*image, 0, 0) == 0xFFFF0000, "masked: opaque pixel");
      check(image && pixel_at(*image, 1, 0) == PIXEL_WHITE, "masked: outline next to inverted pixel");
      check(image && pixel_at(*image, 2, 0) == PIXEL_BLACK, "masked: inverted pixel");
    }

    // Monochrome 8x3: row 0 black/white/transparent/invert, row 1 transparent, row 2 transparent
    {
      // AND mask rows, then XOR mask rows (pitch 1)
      std::uint8_t data[] = {
        0b00111111,  // AND row 0: pixels 0,1 opaque
        0xFF,  // AND row 1
        0xFF,  // AND row 2
        0b01010000,  // XOR row 0: pixel 1 white, pixel 3 inverted
        0x00,  // XOR row 1
        0x00,  // XOR row 2
      };
      auto image = convert_shape(SHAPE_MONOCHROME, 8, 6, 1, 3, 1, data, sizeof(data));
      check(image && image->width == 8 && image->height == 3, "mono: size");
      check(image && pixel_at(*image, 0, 0) == PIXEL_BLACK, "mono: black");
      check(image && pixel_at(*image, 1, 0) == PIXEL_WHITE, "mono: white");
      check(image && pixel_at(*image, 2, 0) == PIXEL_WHITE, "mono: outline left of inverted pixel");
      check(image && pixel_at(*image, 3, 0) == PIXEL_BLACK, "mono: inverted pixel");
      check(image && pixel_at(*image, 4, 1) == PIXEL_WHITE, "mono: diagonal outline");
      check(image && pixel_at(*image, 5, 0) == PIXEL_CLEAR, "mono: transparent");
      check(image && pixel_at(*image, 3, 2) == PIXEL_CLEAR, "mono: far pixel stays transparent");
      check(image && image->hot_x == 3 && image->hot_y == 1, "mono: hotspot");
    }

    // Invalid shapes
    {
      std::vector<std::uint8_t> big(257 * 4 * 2);
      check(!convert_shape(SHAPE_COLOR, 257, 2, 257 * 4, 0, 0, big.data(), big.size()), "too wide");
      check(!convert_shape(SHAPE_COLOR, 2, 2, 8, 0, 0, big.data(), 15), "buffer too small");
      check(!convert_shape(SHAPE_MONOCHROME, 8, 1, 1, 0, 0, big.data(), big.size()), "monochrome without image rows");
      check(!convert_shape(3, 2, 2, 8, 0, 0, big.data(), big.size()), "unknown type");
    }

    // Chunks: 256x256 is 8 chunks of 32 KiB with consistent headers
    image_t large;
    large.width = 256;
    large.height = 256;
    large.hot_x = 7;
    large.hot_y = 9;
    large.bgra.resize(256 * 256 * 4);
    for (std::size_t i = 0; i < large.bgra.size(); i++) {
      large.bgra[i] = (std::uint8_t) (i * 7);
    }
    {
      auto chunks = make_shape_chunks(42, large);
      check(chunks.size() == 8, "chunks: count");
      std::vector<std::uint8_t> reassembled;
      bool headers_ok = true;
      for (std::size_t i = 0; i < chunks.size(); i++) {
        auto p = (const std::uint8_t *) chunks[i].data();
        headers_ok = headers_ok && read_pixel(p) == 42 && p[4] == 0 && p[5] == 1 && p[8] == 7 && p[10] == 9 &&
                     read_pixel(p + 12) == 256 * 256 * 4 && read_pixel(p + 16) == i * CURSOR_MAX_CHUNK &&
                     chunks[i].size() == CURSOR_SHAPE_HEADER_SIZE + CURSOR_MAX_CHUNK;
        reassembled.insert(reassembled.end(), p + CURSOR_SHAPE_HEADER_SIZE, p + chunks[i].size());
      }
      check(headers_ok, "chunks: headers");
      check(reassembled == large.bgra, "chunks: data");
      check(make_state_payload(true, 0x01020304) == std::string("\x01\x04\x03\x02\x01", 5), "state payload");
    }

    // Shape cache: identical shapes keep their id
    shape_cache_t cache;
    image_t small = *convert_shape(SHAPE_COLOR, 1, 1, 4, 0, 0, large.bgra.data(), 4);
    auto id_large = cache.insert(large);
    auto id_small = cache.insert(small);
    check(id_large != 0 && id_small != 0 && id_large != id_small, "cache: new ids");
    check(cache.insert(large) == id_large, "cache: same shape same id");
    check(cache.find(id_small) && *cache.find(id_small) == small, "cache: find");

    // Session updates
    {
      session_state_t state;
      host_cursor_t host {id_small, cache.find(id_small), true};

      check(session_updates(state, host).empty(), "session: nothing without the capability");
      state.granted = true;
      check(session_updates(state, host).empty(), "session: nothing while inactive");

      state.active = true;
      auto messages = session_updates(state, host);
      check(messages.size() == 2 && messages[0].type == PTYPE_CURSOR_SHAPE && messages[1].type == PTYPE_CURSOR_STATE, "session: shape then state");
      check(session_updates(state, host).empty(), "session: no repeats");

      host.visible = false;
      messages = session_updates(state, host);
      check(messages.size() == 1 && messages[0].payload == make_state_payload(false, id_small), "session: visibility only");

      host = {id_large, cache.find(id_large), true};
      messages = session_updates(state, host);
      check(messages.size() == 9 && messages.back().type == PTYPE_CURSOR_STATE, "session: second shape");

      host = {id_small, cache.find(id_small), true};
      messages = session_updates(state, host);
      check(messages.size() == 1 && messages[0].type == PTYPE_CURSOR_STATE, "session: known shape not resent");

      host = {0, nullptr, true};
      messages = session_updates(state, host);
      check(messages.size() == 1 && messages[0].payload == make_state_payload(true, 0), "session: drawn in video");

      // After CURSOR_SESSION_SHAPES other shapes, the first one has to be sent again
      for (std::uint32_t i = 0; i < CURSOR_SESSION_SHAPES; i++) {
        session_updates(state, {1000 + i, cache.find(id_small), true});
      }
      messages = session_updates(state, {id_small, cache.find(id_small), true});
      check(messages.size() == 2 && messages[0].type == PTYPE_CURSOR_SHAPE, "session: evicted shape resent");
      check(state.client_shapes.size() == CURSOR_SESSION_SHAPES, "session: bounded");
    }

    return failures;
  }
}  // namespace streamfork::cursor
