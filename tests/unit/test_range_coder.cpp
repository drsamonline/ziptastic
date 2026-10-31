// test_range_coder.cpp — the binary range coder and bit trees, on their own.
#include <vector>

#include "entropy/range_coder.hpp"
#include "harness.hpp"

using namespace chce;

namespace {
std::vector<uint32_t> random_bits(uint32_t n, uint32_t p0_percent, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint32_t> bits(n);
  for (auto& b : bits) b = rng.below(100) < p0_percent ? 0 : 1;
  return bits;
}
}  // namespace

TEST(bits_round_trip_biased) {
  for (uint32_t p0 : {5u, 50u, 95u}) {
    auto bits = random_bits(20000, p0, p0);
    RangeEncoder enc;
    BitModel m;
    for (uint32_t b : bits) enc.bit(m, b);
    const auto out = enc.finish();

    // A fresh model: the encoder has mutated `m` through every bit, so the
    // decoder must start from the same initial state, not the final one.
    BitModel m2;
    RangeDecoder dec(out);
    CHECK(dec.ok());
    for (size_t i = 0; i < bits.size(); ++i) {
      const uint32_t got = dec.bit(m2);
      CHECK_EQ(got, bits[i]);
      if (got != bits[i]) break;
    }
  }
}

TEST(adaptation_reaches_the_entropy_bound) {
  // A source that is 99% zeros should cost close to 0.08 bits per bit, not 1.
  auto bits = random_bits(200000, 99, 7);
  RangeEncoder enc;
  BitModel m;
  for (uint32_t b : bits) enc.bit(m, b);
  const auto out = enc.finish();
  const double bits_per_symbol = 8.0 * out.size() / bits.size();
  CHECK_MSG(bits_per_symbol < 0.2, "cost " + std::to_string(bits_per_symbol) +
                                       " bits/bit, expected < 0.2");
  CHECK_MSG(bits_per_symbol > 0.02, "impossibly cheap: " +
                                        std::to_string(bits_per_symbol));
}

TEST(many_independent_models) {
  // Each model must adapt independently; a shared-state bug shows up as a
  // desync here.
  const uint32_t nmodels = 8, n = 4000;
  std::vector<BitModel> models(nmodels);
  std::vector<std::vector<uint32_t>> bits(nmodels);
  for (uint32_t k = 0; k < nmodels; ++k) bits[k] = random_bits(n, 10 + k * 11, 100 + k);

  RangeEncoder enc;
  for (uint32_t i = 0; i < n; ++i)
    for (uint32_t k = 0; k < nmodels; ++k) enc.bit(models[k], bits[k][i]);
  const auto out = enc.finish();

  std::vector<BitModel> models2(nmodels);  // fresh, as above
  RangeDecoder dec(out);
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t k = 0; k < nmodels; ++k) {
      const uint32_t got = dec.bit(models2[k]);
      CHECK_EQ(got, bits[k][i]);
      if (got != bits[k][i]) return;
    }
  }
}

TEST(direct_bits_round_trip) {
  chce_test::Rng rng(3);
  std::vector<uint32_t> values;
  std::vector<uint32_t> widths;
  for (int i = 0; i < 500; ++i) {
    const uint32_t w = 1 + static_cast<uint32_t>(rng.below(20));
    widths.push_back(w);
    values.push_back(static_cast<uint32_t>(rng.next()) & ((1u << w) - 1));
  }
  RangeEncoder enc;
  for (size_t i = 0; i < values.size(); ++i) enc.direct(values[i], widths[i]);
  const auto out = enc.finish();

  RangeDecoder dec(out);
  for (size_t i = 0; i < values.size(); ++i) {
    const uint32_t got = dec.direct(widths[i]);
    CHECK_EQ(got, values[i]);
    if (got != values[i]) break;
  }
}

TEST(bit_trees_round_trip) {
  for (uint32_t bits : {1u, 4u, 8u, 9u}) {
    const uint32_t alphabet = 1u << bits;
    chce_test::Rng rng(bits);
    std::vector<uint32_t> syms(3000);
    for (auto& s : syms) s = static_cast<uint32_t>(rng.below(alphabet));

    BitTreeEncoder te(bits);
    RangeEncoder enc;
    for (uint32_t s : syms) te.encode(enc, s);
    const auto out = enc.finish();

    BitTreeDecoder td(bits);
    RangeDecoder dec(out);
    for (uint32_t s : syms) {
      const uint32_t got = td.decode(dec);
      CHECK_EQ(got, s);
      if (got != s) break;
    }
  }
}

TEST(mixed_bits_direct_and_trees) {
  // The shapes the token coder uses: a flag bit, a tree symbol, direct bits.
  chce_test::Rng rng(99);
  RangeEncoder enc;
  BitModel flag;
  BitTreeEncoder lit(8);
  std::vector<uint32_t> flags, lits, extras;
  for (int i = 0; i < 2000; ++i) {
    const uint32_t f = static_cast<uint32_t>(rng.below(3) == 0);
    const uint32_t l = static_cast<uint32_t>(rng.below(256));
    const uint32_t e = static_cast<uint32_t>(rng.below(64));
    flags.push_back(f); lits.push_back(l); extras.push_back(e);
    enc.bit(flag, f);
    if (f) { lit.encode(enc, l); enc.direct(e, 6); }
  }
  const auto out = enc.finish();

  RangeDecoder dec(out);
  BitModel flag2;
  BitTreeDecoder lit2(8);
  for (size_t i = 0; i < flags.size(); ++i) {
    const uint32_t f = dec.bit(flag2);
    CHECK_EQ(f, flags[i]);
    if (f) {
      CHECK_EQ(lit2.decode(dec), lits[i]);
      CHECK_EQ(dec.direct(6), extras[i]);
    }
    if (!dec.ok()) { CHECK(false); return; }
  }
}

TEST(truncated_input_is_reported) {
  auto bits = random_bits(5000, 50, 1);
  RangeEncoder enc;
  BitModel m;
  for (uint32_t b : bits) enc.bit(m, b);
  const auto out = enc.finish();

  for (size_t cut : {size_t{0}, size_t{1}, size_t{4}, out.size() / 2}) {
    std::span<const uint8_t> part(out.data(), cut);
    RangeDecoder dec(part);
    // Too short to initialise: must report failure rather than pretend.
    if (cut < 5) { CHECK(!dec.ok()); continue; }
    BitModel m2;
    for (size_t i = 0; i < bits.size() && dec.ok(); ++i) dec.bit(m2);
    CHECK(!dec.ok());
  }
}

TEST_MAIN()
