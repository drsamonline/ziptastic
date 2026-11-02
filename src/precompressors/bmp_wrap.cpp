// bmp_wrap.cpp — colour-plane split with a per-row delta, verified by rebuilding.
#include "bmp_wrap.hpp"

#include <cstring>

namespace chce {
namespace {

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
int32_t rdi32(const uint8_t* p) { return static_cast<int32_t>(rd32(p)); }
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80u);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}
bool get_varint(std::span<const uint8_t> s, size_t& pos, uint64_t& out) {
  out = 0;
  uint32_t shift = 0;
  while (pos < s.size()) {
    const uint8_t b = s[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

struct Info {
  size_t pixels_at = 0;
  uint32_t width = 0;
  uint32_t height = 0;  // absolute; the sign only says which way up
  uint32_t channels = 0;
  size_t stride = 0;    // bytes per row, including the 4-byte alignment padding
  size_t header_len = 0;
};

bool parse(std::span<const uint8_t> in, Info& info) {
  if (in.size() < 54) return false;
  if (in[0] != 'B' || in[1] != 'M') return false;
  const uint32_t pixels_at = rd32(in.data() + 10);
  const uint32_t dib = rd32(in.data() + 14);
  if (dib < 40) return false;  // BITMAPINFOHEADER or later only
  const int32_t w = rdi32(in.data() + 18);
  const int32_t h = rdi32(in.data() + 22);
  const uint16_t planes = static_cast<uint16_t>(in[26] | (in[27] << 8));
  const uint16_t bits = static_cast<uint16_t>(in[28] | (in[29] << 8));
  const uint32_t compression = rd32(in.data() + 30);
  if (planes != 1) return false;
  if (bits != 24 && bits != 32) return false;
  if (compression != 0) return false;  // BI_RGB only; RLE and bitfields are other jobs
  if (w <= 0 || h == 0) return false;
  if (w > 1 << 20 || h == INT32_MIN) return false;
  const uint32_t channels = bits / 8;
  const size_t row = static_cast<size_t>(w) * channels;
  const size_t stride = (row + 3) & ~static_cast<size_t>(3);
  const size_t height = static_cast<size_t>(h < 0 ? -h : h);
  if (pixels_at < 14 + dib) return false;
  if (pixels_at + stride * height > in.size()) return false;
  info.pixels_at = pixels_at;
  info.width = static_cast<uint32_t>(w);
  info.height = static_cast<uint32_t>(height);
  info.channels = channels;
  info.stride = stride;
  info.header_len = pixels_at;
  return true;
}

}  // namespace

bool bmp_available() { return true; }

bool bmp_likely(std::span<const uint8_t> in) {
  Info info;
  return parse(in, info);
}

bool bmp_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  Info info;
  if (!parse(in, info)) return false;
  const size_t row_bytes = static_cast<size_t>(info.width) * info.channels;

  packed.clear();
  packed.reserve(in.size());
  put_varint(packed, info.header_len);
  put_varint(packed, info.width);
  put_varint(packed, info.height);
  put_varint(packed, info.channels);
  packed.insert(packed.end(), in.begin(), in.begin() + static_cast<long>(info.header_len));

  // Plane by plane, row by row, each pixel stored as the difference from the pixel to
  // its left. The first pixel of a row is a plain difference from zero, so the decoder
  // needs no special case beyond resetting at the row boundary.
  for (uint32_t c = 0; c < info.channels; ++c) {
    for (uint32_t y = 0; y < info.height; ++y) {
      const uint8_t* row = in.data() + info.pixels_at + y * info.stride;
      uint8_t prev = 0;
      for (uint32_t x = 0; x < info.width; ++x) {
        const uint8_t v = row[x * info.channels + c];
        packed.push_back(static_cast<uint8_t>(v - prev));
        prev = v;
      }
    }
    (void)row_bytes;
  }
  return true;
}

bool bmp_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t header_len = 0, width = 0, height = 0, channels = 0;
  if (!get_varint(packed, pos, header_len)) return false;
  if (!get_varint(packed, pos, width)) return false;
  if (!get_varint(packed, pos, height)) return false;
  if (!get_varint(packed, pos, channels)) return false;
  if (channels != 3 && channels != 4) return false;
  if (width == 0 || height == 0 || width > (1u << 20) || height > (1u << 20)) return false;
  if (pos + header_len > packed.size()) return false;

  const size_t stride = (static_cast<size_t>(width) * channels + 3) & ~static_cast<size_t>(3);
  const size_t pixels = stride * static_cast<size_t>(height);
  const size_t planes = static_cast<size_t>(width) * static_cast<size_t>(height) * channels;
  if (pos + header_len + planes > packed.size()) return false;

  out.assign(header_len + pixels, 0);
  std::memcpy(out.data(), packed.data() + pos, header_len);
  pos += static_cast<size_t>(header_len);

  for (size_t c = 0; c < channels; ++c) {
    for (size_t y = 0; y < height; ++y) {
      uint8_t* row = out.data() + header_len + y * stride;
      uint8_t prev = 0;
      for (size_t x = 0; x < width; ++x) {
        const uint8_t d = packed[pos++];
        prev = static_cast<uint8_t>(prev + d);
        row[x * channels + c] = prev;
      }
    }
  }
  return pos == packed.size();
}

}  // namespace chce
