// test_zpaq.cpp — the context-mixing backend must round-trip exactly.
#include <cstring>
#include <vector>

#include "entropy/cm_zpaq.hpp"
#include "harness.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> text(uint32_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over "};
  std::vector<uint8_t> d;
  while (d.size() < n) {
    const char* s = w[rng.below(6)];
    for (const char* p = s; *p; ++p) d.push_back(static_cast<uint8_t>(*p));
  }
  d.resize(n);
  return d;
}
}  // namespace

TEST(round_trips) {
  for (uint32_t n : {1024u, 16384u, 65536u}) {
    auto d = text(n, n);
    std::vector<uint8_t> c;
    if (!zpaq_compress_block(d, c)) continue;  // declined
    CHECK(c.size() < d.size());
    std::vector<uint8_t> out;
    CHECK_MSG(zpaq_decompress_block(c, n, out), "zpaq decode failed at " + std::to_string(n));
    CHECK_MSG(out == d, "zpaq round-trip failed at " + std::to_string(n));
  }
}

TEST(rejects_corruption) {
  auto d = text(8192, 3);
  std::vector<uint8_t> c;
  if (!zpaq_compress_block(d, c)) return;
  std::vector<uint8_t> bad = c;
  bad[bad.size() / 2] ^= 0xFF;
  std::vector<uint8_t> out;
  // Either it refuses, or it produces something that is not the original. What
  // it must not do is claim success with the wrong bytes.
  const bool ok = zpaq_decompress_block(bad, static_cast<uint32_t>(d.size()), out);
  if (ok) CHECK_MSG(out == d, "corrupt payload decoded to wrong bytes but reported success");
}

TEST_MAIN()
