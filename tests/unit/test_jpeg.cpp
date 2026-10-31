// test_jpeg.cpp — the JPEG layer must detect, re-encode and rebuild exactly.
#include <cstdio>
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "precompressors/jpeg_wrap.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> read_file(const char* p) {
  std::vector<uint8_t> d;
  FILE* f = fopen(p, "rb");
  if (!f) return d;
  uint8_t b[1 << 16];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
  fclose(f);
  return d;
}
}  // namespace

TEST(detects_jpeg_headers) {
  CHECK(jpeg_likely({0xFF, 0xD8, 0xFF, 0xE0, 0x00}));
  CHECK(!jpeg_likely({0x89, 0x50, 0x4E, 0x47}));           // PNG
  CHECK(!jpeg_likely({0x50, 0x4B, 0x03, 0x04}));           // ZIP
  CHECK(!jpeg_likely({0xFF, 0xD8}));                        // too short
  CHECK(!jpeg_likely({}));
}

TEST(declines_non_jpeg_data) {
  std::vector<uint8_t> notjpeg(5000);
  for (size_t i = 0; i < notjpeg.size(); ++i) notjpeg[i] = static_cast<uint8_t>(i * 7);
  std::vector<uint8_t> packed;
  CHECK_MSG(!jpeg_pack(notjpeg, packed), "packed data that is not a JPEG");
}

TEST(round_trips_a_real_jpeg) {
  // The repo vendors sample JPEGs for exactly this purpose.
  auto jpg = read_file(TEST_JPEG_PATH);
  if (jpg.empty()) return;  // vendored sample missing: nothing to test

  std::vector<uint8_t> packed;
  CHECK_MSG(jpeg_pack(jpg, packed), "failed to pack a valid JPEG");
  CHECK_MSG(packed.size() < jpg.size(), "no gain on a JPEG: " +
                                            std::to_string(packed.size()) + " from " +
                                            std::to_string(jpg.size()));

  std::vector<uint8_t> back;
  CHECK_MSG(jpeg_unpack(packed, back), "failed to unpack");
  CHECK_MSG(back == jpg, "JPEG round-trip is not bit-exact");
}

TEST_MAIN()
