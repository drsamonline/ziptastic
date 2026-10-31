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

// Independent states coded round-robin. A single-state rANS is a serial
// dependency chain — each symbol's state depends on the previous one — which
// leaves the CPU's out-of-order machinery with nothing to overlap. Four lanes
// break the chain and cost nothing in ratio (each lane sees the same tables).
inline constexpr uint32_t kRansLanes = 4;

class RansEncoder {
 public:
  // `index` is the symbol's position in the LOGICAL sequence. The lane is
  // index % kRansLanes, which is why it must be passed rather than counted:
  // rANS encodes backwards, so a call counter would assign lanes in reverse and
  // desynchronise from the decoder, which reads forwards.
  void put(const RansTable& t, uint32_t sym, uint64_t index);
  void set_single_lane() { single_lane_ = true; }
  // Writes the final state and returns the payload in decoder order.
  std::vector<uint8_t> finish();
  size_t size() const { return buf_.size(); }

 private:
  uint32_t state_[kRansLanes] = {kRansL, kRansL, kRansL, kRansL};
  bool single_lane_ = false;
  std::vector<uint8_t> buf_;  // reverse order until finish()
};

class RansDecoder {
 public:
  // `payload` must outlive the decoder.
  explicit RansDecoder(std::span<const uint8_t> payload);
  bool ok() const { return ok_; }
  // Lanes advance round-robin automatically, matching the encoder's symbol
  // order. Call set_single_lane() for a stream coded entirely on lane 0 (the
  // adaptive model does that).
  uint32_t get(const RansTable& t);
  void set_single_lane() { single_lane_ = true; }

 private:
  uint8_t next();
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  uint32_t state_[kRansLanes] = {};
  // Decode order is forward, so a call counter is the logical index here.
  uint32_t lane_next_ = 0;
  bool single_lane_ = false;
  bool ok_ = true;
};

}  // namespace chce
