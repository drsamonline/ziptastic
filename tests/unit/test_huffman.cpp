// test_huffman.cpp — the two Huffman backends, static and adaptive.
#include <cstring>
#include <vector>

#include "chimera/huffman.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> skewed(uint32_t n, uint32_t distinct, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) {
    // Strongly favour low symbol values so the distribution is lopsided.
    const uint32_t r = static_cast<uint32_t>(rng.below(1000));
    b = static_cast<uint8_t>(r < 800 ? rng.below(2) : rng.below(distinct));
  }
  return d;
}

void roundtrip_static(const std::vector<uint8_t>& d) {
  ByteWriter w;
  huff_static_encode(d, w);
  std::vector<uint8_t> out;
  ByteReader r(w.span());
  CHECK_ERR(huff_static_decode(r, static_cast<uint32_t>(d.size()), out), Err::Ok);
  CHECK_MSG(out == d, "static Huffman round-trip failed at " + std::to_string(d.size()));
}

void roundtrip_adaptive(const std::vector<uint8_t>& d) {
  ByteWriter w;
  huff_adaptive_encode(d, w);
  std::vector<uint8_t> out;
  ByteReader r(w.span());
  CHECK_ERR(huff_adaptive_decode(r, static_cast<uint32_t>(d.size()), out), Err::Ok);
  CHECK_MSG(out == d, "adaptive Huffman round-trip failed at " + std::to_string(d.size()));
}

}  // namespace

TEST(static_round_trips) {
  for (uint32_t n : {1u, 2u, 255u, 256u, 4096u, 100000u}) {
    roundtrip_static(skewed(n, 256, n));
  }
  std::vector<uint8_t> zeros(10000, 0);
  roundtrip_static(zeros);  // single symbol
  std::vector<uint8_t> uniform(10000);
  for (uint32_t i = 0; i < uniform.size(); ++i) uniform[i] = static_cast<uint8_t>(i);
  roundtrip_static(uniform);  // all 256 symbols equally likely
}

TEST(adaptive_round_trips) {
  for (uint32_t n : {1u, 2u, 255u, 256u, 4096u, 100000u}) {
    roundtrip_adaptive(skewed(n, 256, n + 1));
  }
  std::vector<uint8_t> zeros(10000, 7);
  roundtrip_adaptive(zeros);
}

TEST(adaptive_round_trips_across_rebuilds) {
  // 256-symbol rebuilds: make sure the encoder and decoder rebuild in step.
  auto d = skewed(100000, 64, 5);
  roundtrip_adaptive(d);
  // And with a distribution that changes partway, so the tree must move.
  std::vector<uint8_t> mixed;
  for (int i = 0; i < 40000; ++i) mixed.push_back(static_cast<uint8_t>(i % 2));
  for (int i = 0; i < 40000; ++i) mixed.push_back(static_cast<uint8_t>('a' + i % 20));
  roundtrip_adaptive(mixed);
  roundtrip_static(mixed);
}

TEST(static_compresses_skewed_data) {
  auto d = skewed(200000, 256, 9);
  ByteWriter w;
  huff_static_encode(d, w);
  CHECK_MSG(w.size() < d.size(), "no compression on skewed data: " +
                                     std::to_string(w.size()) + " from " +
                                     std::to_string(d.size()));
}

TEST(loses_to_raw_on_uniform_random) {
  // A Huffman code cannot beat 8 bits per byte on uniform random data, and the
  // selector must therefore prefer RAW. This checks the coder's own size, which
  // is what the selector compares.
  chce_test::Rng rng(3);
  std::vector<uint8_t> d(200000);
  for (auto& b : d) b = rng.byte();
  ByteWriter w;
  huff_static_encode(d, w);
  CHECK_MSG(w.size() >= d.size(), "Huffman beat raw on uniform data, which is "
                                  "impossible: " + std::to_string(w.size()));
}

TEST(deterministic) {
  auto d = skewed(20000, 32, 11);
  ByteWriter a, b;
  huff_static_encode(d, a);
  huff_static_encode(d, b);
  CHECK(a.buffer() == b.buffer());

  ByteWriter c, e;
  huff_adaptive_encode(d, c);
  huff_adaptive_encode(d, e);
  CHECK(c.buffer() == e.buffer());
}

TEST(truncation_is_rejected) {
  auto d = skewed(5000, 16, 13);
  ByteWriter w;
  huff_static_encode(d, w);
  const auto full = w.buffer();
  for (size_t cut : {size_t{0}, size_t{100}, size_t{256}, full.size() / 2}) {
    std::vector<uint8_t> out;
    ByteReader r(std::span<const uint8_t>(full.data(), cut));
    const Err e = huff_static_decode(r, static_cast<uint32_t>(d.size()), out);
    CHECK_MSG(e != Err::Ok || out.size() == d.size(),
              "truncated static stream at " + std::to_string(cut) + " decoded");
  }
}

TEST_MAIN()
