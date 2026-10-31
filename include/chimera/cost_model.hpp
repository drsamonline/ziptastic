// cost_model.hpp — the pluggable price function the parser optimises against.
//
// The parser is a shortest-path search: every candidate token has a price, and
// the DP finds the cheapest path through the block. Swapping the cost model
// changes the parse without touching the search — which is how Phase 4 (rANS),
// Phase 6 (coarse-match discount) and later tuning all reuse this file.
//
// Prices are in **1/16 bit units**, the same fixed point LZMA uses. 8 bits is
// 128. Integer prices keep the DP exact and reproducible across platforms,
// which matters because the encoder must be deterministic byte for byte.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace chce {

// Where a match came from. Phase 6 adds COARSE (long-range block hashes) and
// gives it a distance discount, because a coarse match is stored differently.
enum class MatchSource : uint8_t {
  Fine = 0,    // binary-tree finder, 64 MiB window
  Coarse = 1,  // block-hash index, unbounded history
};

// Shortest length the parser will consider as a match.
inline constexpr uint32_t kMinMatchLen = 2;
// Longest match the format can express (LZMA's limit, so the two are
// comparable on the same corpus).
inline constexpr uint32_t kMaxMatchLen = 273;

class CostModel {
 public:
  virtual ~CostModel() = default;

  // Price of coding `byte` as a literal given a context selector.
  virtual uint32_t literal_price(uint8_t ctx, uint8_t byte) const = 0;

  // Price of coding a match of `len` bytes at `dist` bytes back.
  virtual uint32_t match_price(uint32_t len, uint32_t dist,
                               MatchSource src) const = 0;
};

// A static, data-fitted model: literal prices come from a real order-1 byte
// distribution measured over the input, and match prices from the slot layout
// an LZ coder uses (a length slot plus extra bits, a distance slot plus extra
// bits).
//
// It is deliberately *static* rather than adaptive: Phase 3 has no entropy
// coder yet, and a static model fitted to the data is a fair proxy for what an
// adaptive one converges to, without pretending to be the coder itself.
class StaticCostModel final : public CostModel {
 public:
  // Number of literal contexts: the high nibble of the previous byte.
  static constexpr uint32_t kContexts = 16;

  // Builds the model over `data`. `coarse_discount` is subtracted from coarse
  // match prices (0 until Phase 6 turns the coarse index on).
  explicit StaticCostModel(std::span<const uint8_t> data,
                           uint32_t coarse_discount = 0);

  uint32_t literal_price(uint8_t ctx, uint8_t byte) const override;
  uint32_t match_price(uint32_t len, uint32_t dist, MatchSource src) const override;

  // Context selector for the byte at `pos` in the modelled data.
  static uint8_t context_for(std::span<const uint8_t> data, uint32_t pos);

 private:
  // prices_[ctx][byte] in 1/16 bit units.
  std::array<std::array<uint16_t, 256>, kContexts> prices_{};
  uint32_t coarse_discount_ = 0;
};

}  // namespace chce
