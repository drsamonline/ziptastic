// test_ppmd.cpp — the PPMd backend must round-trip exactly.
#include <cstring>
#include <vector>

#include "entropy/ppmd.hpp"
#include "harness.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> text(uint32_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
                            "over ", "the ", "lazy ", "dog. "};
  std::vector<uint8_t> d;
  while (d.size() < n) {
    const char* s = w[rng.below(9)];
    for (const char* p = s; *p; ++p) d.push_back(static_cast<uint8_t>(*p));
  }
  d.resize(n);
  return d;
}
}  // namespace

TEST(round_trips_on_text) {
  for (uint32_t n : {256u, 4096u, 65536u}) {
    auto d = text(n, n);
    std::vector<uint8_t> c;
    if (!ppmd_compress_block(d, c)) continue;
    CHECK(c.size() < d.size());
    std::vector<uint8_t> out;
    CHECK_ERR(ppmd_decompress_block(c, n, out), Err::Ok);
    CHECK_MSG(out == d, "PPMd round-trip failed at " + std::to_string(n));
  }
}

TEST(round_trips_on_binary) {
  chce_test::Rng rng(3);
  std::vector<uint8_t> d(32768);
  for (uint32_t i = 0; i < d.size(); ++i) d[i] = static_cast<uint8_t>((i / 16) & 0xFF);
  std::vector<uint8_t> c;
  if (!ppmd_compress_block(d, c)) return;
  std::vector<uint8_t> out;
  CHECK_ERR(ppmd_decompress_block(c, static_cast<uint32_t>(d.size()), out), Err::Ok);
  CHECK(out == d);
}

TEST(rejects_truncated) {
  auto d = text(8192, 5);
  std::vector<uint8_t> c;
  if (!ppmd_compress_block(d, c)) return;
  std::vector<uint8_t> out;
  const Err e = ppmd_decompress_block(
      std::span<const uint8_t>(c.data(), c.size() / 2),
      static_cast<uint32_t>(d.size()), out);
  CHECK_MSG(e != Err::Ok || out.size() == d.size(),
            "truncated PPMd stream decoded to the wrong length");
}

TEST_MAIN()
