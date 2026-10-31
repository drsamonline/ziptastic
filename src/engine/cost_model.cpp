// cost_model.cpp — static order-1 literal prices and slot-based match prices.
#include "chimera/cost_model.hpp"

#include <cmath>
#include <cstring>

namespace chce {
namespace {

// Smallest price any symbol can have, in 1/16 bit units. A symbol that looks
// free would let the DP pile up infinite zero-cost matches, so every price has
// a floor.
constexpr uint32_t kMinPrice = 1;

// Price of a symbol with probability p in 1/16 bit units.
uint32_t price_of(double p) {
  if (!(p > 0.0)) return 16u * 24u;  // unseen: expensive but finite
  const double bits = -std::log2(p);
  const double units = bits * 16.0;
  if (units < static_cast<double>(kMinPrice)) return kMinPrice;
  if (units > 16.0 * 24.0) return 16u * 24u;
  return static_cast<uint32_t>(units + 0.5);
}

uint32_t ceil_log2(uint32_t v) {
  uint32_t bits = 0;
  while ((1u << bits) < v && bits < 32) ++bits;
  return bits;
}

}  // namespace

StaticCostModel::StaticCostModel(std::span<const uint8_t> data,
                                 uint32_t coarse_discount)
    : coarse_discount_(coarse_discount) {
  // Count bytes per context. Context 0 is also the "no previous byte" context,
  // so the first byte of a block is priced from the same table.
  std::array<std::array<uint32_t, 256>, kContexts> counts{};
  std::array<uint32_t, kContexts> totals{};

  for (size_t i = 0; i < data.size(); ++i) {
    const uint8_t ctx = i == 0 ? 0 : static_cast<uint8_t>(data[i - 1] >> 4);
    ++counts[ctx][data[i]];
    ++totals[ctx];
  }

  for (uint32_t c = 0; c < kContexts; ++c) {
    // Back off to the global distribution when a context is thin, so a rare
    // context does not produce absurd prices from a handful of samples.
    if (totals[c] < 64) {
      for (uint32_t b = 0; b < 256; ++b) {
        prices_[c][b] = static_cast<uint16_t>(price_of(1.0 / 256.0));
      }
      continue;
    }
    const double total = static_cast<double>(totals[c]);
    for (uint32_t b = 0; b < 256; ++b) {
      // Add a half-count smoothing term: a byte that never appears in this
      // context is still possible, just expensive.
      const double p = (counts[c][b] + 0.5) / (total + 128.0);
      prices_[c][b] = static_cast<uint16_t>(price_of(p));
    }
  }
}

uint8_t StaticCostModel::context_for(std::span<const uint8_t> data, uint32_t pos) {
  return pos == 0 ? 0 : static_cast<uint8_t>(data[pos - 1] >> 4);
}

uint32_t StaticCostModel::literal_price(uint8_t ctx, uint8_t byte) const {
  return prices_[ctx & (kContexts - 1)][byte];
}

uint32_t StaticCostModel::match_price(uint32_t len, uint32_t dist,
                                      MatchSource src) const {
  if (len < kMinMatchLen) return 16u * 24u;

  // Length: a slot (how many bits the length needs) plus the extra bits.
  const uint32_t len_bits = 4 + ceil_log2(len - 1 + 1);
  // Distance: a slot plus the extra bits, which dominate for long distances.
  const uint32_t dist_bits = 6 + ceil_log2(dist);

  uint32_t price = 16u * (len_bits + dist_bits);
  if (src == MatchSource::Coarse && coarse_discount_ > 0) {
    price = price > coarse_discount_ ? price - coarse_discount_ : kMinPrice;
  }
  return price;
}

}  // namespace chce
