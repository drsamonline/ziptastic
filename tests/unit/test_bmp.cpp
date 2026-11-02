// test_bmp.cpp — the colour-plane split must be reversible, and must decline what it
// cannot handle rather than producing something that decodes to almost the right image.
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"
#include "precompressors/bmp_wrap.hpp"

using namespace chce;

namespace {

// A 24-bit BI_RGB BMP with the given pixel function, bottom-up as the format requires.
std::vector<uint8_t> make_bmp(int32_t w, int32_t h, uint32_t (*pix)(int32_t, int32_t),
                              uint16_t bits = 24) {
  const size_t ch = bits / 8;
  const size_t row = static_cast<size_t>(w) * ch;
  const size_t stride = (row + 3) & ~static_cast<size_t>(3);
  std::vector<uint8_t> data(stride * static_cast<size_t>(h), 0);
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const uint32_t v = pix(x, y);
      uint8_t* p = data.data() + static_cast<size_t>(h - 1 - y) * stride + x * ch;
      p[0] = static_cast<uint8_t>(v);
      p[1] = static_cast<uint8_t>(v >> 8);
      p[2] = static_cast<uint8_t>(v >> 16);
      if (ch == 4) p[3] = 0xFF;
    }
  }
  const uint32_t pixels_at = 54;
  std::vector<uint8_t> out(54, 0);
  out[0] = 'B';
  out[1] = 'M';
  auto put32 = [&out](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  put32(2, static_cast<uint32_t>(pixels_at + data.size()));
  put32(10, pixels_at);
  put32(14, 40);
  put32(18, static_cast<uint32_t>(w));
  put32(22, static_cast<uint32_t>(h));
  out[26] = 1;
  out[28] = static_cast<uint8_t>(bits);
  put32(30, 0);  // BI_RGB
  put32(34, static_cast<uint32_t>(data.size()));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

uint32_t gradient(int32_t x, int32_t y) {
  return static_cast<uint32_t>((x * 3 + y * 5) & 0xFF) |
         (static_cast<uint32_t>((x * 7) & 0xFF) << 8) |
         (static_cast<uint32_t>((x + y) & 0xFF) << 16);
}

void round_trip(const std::vector<uint8_t>& bmp) {
  std::vector<uint8_t> packed, back;
  CHECK_MSG(bmp_pack(bmp, packed), "declined a valid BMP");
  CHECK_MSG(bmp_unpack(packed, back), "unpack failed");
  CHECK_MSG(back == bmp, "round-trip is not byte-exact");
}

}  // namespace

TEST(round_trips_a_24_bit_bmp) { round_trip(make_bmp(37, 23, gradient)); }

TEST(round_trips_a_32_bit_bmp) { round_trip(make_bmp(16, 9, gradient, 32)); }

TEST(round_trips_an_odd_width_with_padding) {
  // 37 pixels x 3 bytes = 111, padded to 112: the stride is not the row.
  round_trip(make_bmp(37, 5, gradient));
}

// The payload replaces the row padding with a header, so it is the same size to within
// a few dozen bytes. Whether the transform is WORTH taking is the gate's decision, not
// this function's — a flat sprite sheet produces a payload of the same size and still
// compresses worse, which is why the stage trials it.
TEST(the_payload_is_the_same_size) {
  auto bmp = make_bmp(64, 64, gradient);
  std::vector<uint8_t> packed;
  CHECK_MSG(bmp_pack(bmp, packed), "declined a valid BMP");
  CHECK_MSG(packed.size() <= bmp.size() + 64, "payload grew by more than its header");
}

TEST(declines_a_compressed_bmp) {
  auto bmp = make_bmp(32, 32, gradient);
  bmp[30] = 1;  // BI_RLE8: a different transform entirely
  std::vector<uint8_t> packed;
  CHECK_MSG(!bmp_pack(bmp, packed), "accepted an RLE BMP");
}

TEST(declines_a_non_bmp) {
  const std::string prose = "This is a sentence about a bitmap, not a bitmap.";
  std::vector<uint8_t> packed;
  CHECK_MSG(!bmp_pack(std::vector<uint8_t>(prose.begin(), prose.end()), packed),
            "accepted prose as a BMP");
}

TEST(declines_a_truncated_bmp) {
  auto bmp = make_bmp(32, 32, gradient);
  bmp.resize(bmp.size() - 100);
  std::vector<uint8_t> packed;
  CHECK_MSG(!bmp_pack(bmp, packed), "accepted a truncated BMP");
}

TEST(unpack_rejects_a_damaged_payload) {
  auto bmp = make_bmp(32, 32, gradient);
  std::vector<uint8_t> packed, back;
  CHECK_MSG(bmp_pack(bmp, packed), "declined a valid BMP");
  packed.resize(packed.size() - 10);
  CHECK_MSG(!bmp_unpack(packed, back), "accepted a truncated payload");
}

TEST_MAIN()
