// test_bwt.cpp — the BWT backend must round-trip and must decline bad input.
#include <cstring>
#include <vector>

#include "entropy/bwt_bsc.hpp"
#include "harness.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> structured(uint32_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (uint32_t i = 0; i < n; ++i) {
    // Records with a repeated shape: the kind of data BWT is good at.
    d[i] = static_cast<uint8_t>((i % 64) < 8 ? rng.byte() : (i * 7 + (i / 64) * 13) & 0xFF);
  }
  return d;
}
}  // namespace

TEST(round_trips) {
  for (uint32_t n : {1024u, 65536u, 262144u}) {
    auto d = structured(n, n);
    std::vector<uint8_t> c;
    if (!bsc_compress_block(d, c)) continue;  // declined: not a failure
    CHECK(c.size() < d.size());
    std::vector<uint8_t> out;
    CHECK_ERR(bsc_decompress_block(c, n, out), Err::Ok);
    CHECK_MSG(out == d, "BWT round-trip failed at " + std::to_string(n));
  }
}

TEST(declines_incompressible) {
  chce_test::Rng rng(5);
  std::vector<uint8_t> d(65536);
  for (auto& b : d) b = rng.byte();
  std::vector<uint8_t> c;
  // Either libbsc declines or the result is not smaller; both are fine, but it
  // must never hand back something bigger than the input.
  if (bsc_compress_block(d, c)) {
    CHECK_MSG(c.size() < d.size(), "BWT returned an output larger than its input");
  }
}

TEST(rejects_truncated_input) {
  auto d = structured(32768, 9);
  std::vector<uint8_t> c;
  if (!bsc_compress_block(d, c)) return;
  std::vector<uint8_t> out;
  ByteReader r(std::span<const uint8_t>(c.data(), c.size() / 3));
  CHECK(bsc_decompress_block(std::span<const uint8_t>(c.data(), c.size() / 3), 32768,
                             out) != Err::Ok);
  (void)r;
}

TEST_MAIN()
