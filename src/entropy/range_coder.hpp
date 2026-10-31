// range_coder.hpp — binary range coder with adaptive bit models.
//
// Why this exists: rANS is a stack, so the decoder pops symbols in reverse
// encoding order and a per-symbol adaptive model cannot be mirrored. A range
// coder is FIFO — bits go out and come back in the same order — so the encoder
// and decoder can update a probability after *every* bit and stay in lockstep.
// That is what makes the adaptive cadence the spec asks for reachable at all.
//
// The formulation is the standard LZMA one: 32-bit range, 11-bit probabilities,
// 5-bit adaptation shift, carry handling through a cache byte. It is not novel
// and does not try to be; the value is that it is exact, bounded, and its state
// is a plain value, so it can be checked against a reference implementation.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

// Probability that the next bit is 0, out of kBitModelTotal.
inline constexpr uint16_t kBitModelTotal = 1u << 11;  // 2048
inline constexpr uint32_t kNumMoveBits = 5;
inline constexpr uint32_t kTopValue = 1u << 24;

// One adaptive binary model. Initialised to "even" and updated after each use:
// the probability moves toward the observed bit by 1/32 of the gap, which is a
// window of roughly 32 observations.
struct BitModel {
  uint16_t prob = kBitModelTotal / 2;
};

class RangeEncoder {
 public:
  void bit(BitModel& m, uint32_t bit);
  // Uniform bits, no model: exactly nbits bits each.
  void direct(uint32_t value, uint32_t nbits);
  std::vector<uint8_t> finish();

 private:
  void shift_low();
  uint64_t low_ = 0;
  uint32_t range_ = 0xFFFFFFFFu;
  uint8_t cache_ = 0;
  uint64_t cache_size_ = 1;
  std::vector<uint8_t> out_;
};

class RangeDecoder {
 public:
  explicit RangeDecoder(std::span<const uint8_t> in);
  bool ok() const { return ok_; }
  uint32_t bit(BitModel& m);
  uint32_t direct(uint32_t nbits);

 private:
  uint8_t next();
  uint32_t range_ = 0xFFFFFFFFu;
  uint32_t code_ = 0;
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// A bit-tree codes a symbol of `bits` bits MSB-first through 2^bits-1 models.
// It is the usual way to turn a binary coder into a symbol coder, and it adapts
// per node, so each context learns its own distribution.
class BitTreeEncoder {
 public:
  explicit BitTreeEncoder(uint32_t bits);
  void encode(RangeEncoder& rc, uint32_t symbol);
  uint32_t bits() const { return bits_; }

 private:
  uint32_t bits_;
  std::vector<BitModel> models_;
};

class BitTreeDecoder {
 public:
  explicit BitTreeDecoder(uint32_t bits);
  uint32_t decode(RangeDecoder& rc);
  uint32_t bits() const { return bits_; }

 private:
  uint32_t bits_;
  std::vector<BitModel> models_;
};

}  // namespace chce
