// rans.hpp — range Asymmetric Numeral Systems, byte-renormalised, 12-bit tables.
//
// This is the primary CHCE entropy backend. It follows the well-known ryg_rans
// formulation (public domain): a 32-bit state, byte-at-a-time renormalisation,
// and per-context frequency tables of 4096 slots. Phase 4 uses *static* tables
// measured on the block; Phase 5 turns them adaptive.
//
// The encoder emits bytes back to front, so the decoder can read forwards.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

inline constexpr uint32_t kRansScaleBits = 12;
inline constexpr uint32_t kRansTotal = 1u << kRansScaleBits;  // 4096
inline constexpr uint32_t kRansL = 1u << 23;

// One context's probability table: normalised frequencies plus the two lookup
// structures the decoder needs.
class RansTable {
 public:
  void build(const uint32_t* counts, uint32_t alphabet);

  // False when the table carries no information (a single symbol).
  bool valid() const { return alphabet_ > 0; }

  uint32_t freq(uint32_t sym) const { return freq_[sym]; }
  uint32_t start(uint32_t sym) const { return cum_[sym]; }

  // Decoder-side: maps a 12-bit slot to the symbol that owns it.
  uint32_t symbol_at(uint32_t slot) const { return slot_sym_[slot]; }

  uint32_t alphabet() const { return alphabet_; }

  // Compact serialisation: only the symbols that occur are written.
  void write(ByteWriter& w) const;
  Err read(ByteReader& r);

  size_t memory_bytes() const;

 private:
  uint32_t alphabet_ = 0;
  std::vector<uint16_t> freq_;
  std::vector<uint16_t> cum_;
  std::vector<uint8_t> slot_sym_;  // kRansTotal entries
};

class RansEncoder {
 public:
  void put(const RansTable& t, uint32_t sym);
  // Writes the final state and returns the payload in decoder order.
  std::vector<uint8_t> finish();
  size_t size() const { return buf_.size(); }

 private:
  uint32_t state_ = kRansL;
  std::vector<uint8_t> buf_;  // reverse order until finish()
};

class RansDecoder {
 public:
  // `payload` must outlive the decoder.
  explicit RansDecoder(std::span<const uint8_t> payload);
  bool ok() const { return ok_; }
  uint32_t get(const RansTable& t);

 private:
  uint8_t next();
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  uint32_t state_ = 0;
  bool ok_ = true;
};

}  // namespace chce
