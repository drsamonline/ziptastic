// range_coder.cpp — the LZMA-style binary range coder and bit trees.
#include "range_coder.hpp"

namespace chce {

void RangeEncoder::shift_low() {
  // The classic carry dance: whenever the top byte of `low_` is settled, emit
  // it, flushing any pending 0xFF bytes with it.
  if (static_cast<uint32_t>(low_) < 0xFF000000u || (low_ >> 32) != 0) {
    const uint8_t carry = static_cast<uint8_t>(low_ >> 32);
    do {
      out_.push_back(static_cast<uint8_t>(cache_ + carry));
      cache_ = 0xFF;
    } while (--cache_size_ != 0);
    cache_ = static_cast<uint8_t>(low_ >> 24);
  }
  ++cache_size_;
  low_ = (low_ & 0x00FFFFFFu) << 8;
}

void RangeEncoder::bit(BitModel& m, uint32_t bit_value) {
  const uint32_t bound = (range_ >> 11) * m.prob;
  if (bit_value == 0) {
    range_ = bound;
    m.prob = static_cast<uint16_t>(m.prob + ((kBitModelTotal - m.prob) >> kNumMoveBits));
  } else {
    low_ += bound;
    range_ -= bound;
    m.prob = static_cast<uint16_t>(m.prob - (m.prob >> kNumMoveBits));
  }
  while (range_ < kTopValue) {
    range_ <<= 8;
    shift_low();
  }
}

void RangeEncoder::direct(uint32_t value, uint32_t nbits) {
  for (uint32_t i = nbits; i-- > 0;) {
    range_ >>= 1;
    if ((value >> i) & 1u) low_ += range_;
    while (range_ < kTopValue) {
      range_ <<= 8;
      shift_low();
    }
  }
}

std::vector<uint8_t> RangeEncoder::finish() {
  for (int i = 0; i < 5; ++i) shift_low();
  return std::move(out_);
}

RangeDecoder::RangeDecoder(std::span<const uint8_t> in) : in_(in) {
  if (in_.size() < 5) {
    ok_ = false;
    return;
  }
  // The encoder's first byte is always the initial cache (zero); reading five
  // bytes into a 32-bit code drops it, which is the standard arrangement.
  for (int i = 0; i < 5; ++i) code_ = (code_ << 8) | next();
  if (!ok_) return;
  if (code_ == 0xFFFFFFFFu) ok_ = false;  // cannot be produced by the encoder
}

uint8_t RangeDecoder::next() {
  if (pos_ >= in_.size()) {
    ok_ = false;
    return 0;
  }
  return in_[pos_++];
}

uint32_t RangeDecoder::bit(BitModel& m) {
  const uint32_t bound = (range_ >> 11) * m.prob;
  uint32_t bit_value;
  if (code_ < bound) {
    range_ = bound;
    m.prob = static_cast<uint16_t>(m.prob + ((kBitModelTotal - m.prob) >> kNumMoveBits));
    bit_value = 0;
  } else {
    code_ -= bound;
    range_ -= bound;
    m.prob = static_cast<uint16_t>(m.prob - (m.prob >> kNumMoveBits));
    bit_value = 1;
  }
  while (range_ < kTopValue) {
    range_ <<= 8;
    code_ = (code_ << 8) | next();
  }
  return bit_value;
}

uint32_t RangeDecoder::direct(uint32_t nbits) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < nbits; ++i) {
    range_ >>= 1;
    const uint32_t t = (code_ - range_) >> 31;  // 0 when code_ >= range_
    if (t == 0) code_ -= range_;
    result = (result << 1) + (1 - t);
    while (range_ < kTopValue) {
      range_ <<= 8;
      code_ = (code_ << 8) | next();
    }
  }
  return result;
}

BitTreeEncoder::BitTreeEncoder(uint32_t bits)
    : bits_(bits), models_(static_cast<size_t>(1) << bits) {}

void BitTreeEncoder::encode(RangeEncoder& rc, uint32_t symbol) {
  uint32_t m = 1;
  for (uint32_t i = bits_; i-- > 0;) {
    const uint32_t b = (symbol >> i) & 1u;
    rc.bit(models_[m], b);
    m = (m << 1) | b;
  }
}

BitTreeDecoder::BitTreeDecoder(uint32_t bits)
    : bits_(bits), models_(static_cast<size_t>(1) << bits) {}

uint32_t BitTreeDecoder::decode(RangeDecoder& rc) {
  uint32_t m = 1;
  for (uint32_t i = 0; i < bits_; ++i) {
    m = (m << 1) | rc.bit(models_[m]);
  }
  return m - (1u << bits_);
}

}  // namespace chce
