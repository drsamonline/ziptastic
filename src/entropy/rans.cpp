// rans.cpp — table construction, normalisation and the coder itself.
#include "rans.hpp"

#include <algorithm>
#include <cstring>

namespace chce {

void RansTable::build(const uint32_t* counts, uint32_t alphabet) {
  alphabet_ = alphabet;
  freq_.assign(alphabet, 0);
  cum_.assign(alphabet, 0);
  slot_sym_.assign(kRansTotal, 0);

  // Collect what actually occurs.
  std::vector<uint32_t> used;
  uint64_t total = 0;
  for (uint32_t s = 0; s < alphabet; ++s) {
    if (counts[s]) {
      used.push_back(s);
      total += counts[s];
    }
  }
  if (used.empty()) {
    // Nothing was coded in this context. Give it a single symbol so the
    // structure stays well-formed; it will simply never be selected.
    freq_[0] = kRansTotal;
    cum_[0] = 0;
    std::fill(slot_sym_.begin(), slot_sym_.end(), uint8_t{0});
    return;
  }
  if (used.size() == 1) {
    freq_[used[0]] = kRansTotal;
    cum_[used[0]] = 0;
    std::fill(slot_sym_.begin(), slot_sym_.end(),
              static_cast<uint8_t>(used[0]));
    return;
  }

  // Scale to the 12-bit total, every used symbol at least one slot.
  uint32_t sum = 0;
  for (uint32_t s : used) {
    uint32_t f = static_cast<uint32_t>(
        (static_cast<uint64_t>(counts[s]) * kRansTotal + total / 2) / total);
    if (f == 0) f = 1;
    freq_[s] = static_cast<uint16_t>(f);
    sum += f;
  }

  // Repair the rounding so the frequencies sum to exactly kRansTotal. Units are
  // moved to or from the most probable symbol one at a time, and never taken
  // from a symbol that is already at 1 — an earlier version subtracted the
  // whole difference at once and clamped, which left the total wrong and then
  // overran the 4096-slot decode table (a heap overwrite that aborted the
  // process on mozilla).
  while (sum != kRansTotal) {
    uint32_t biggest = used[0];
    for (uint32_t s : used) {
      if (freq_[s] > freq_[biggest]) biggest = s;
    }
    if (sum > kRansTotal) {
      if (freq_[biggest] <= 1) break;  // cannot take without losing a symbol
      freq_[biggest] = static_cast<uint16_t>(freq_[biggest] - 1);
      --sum;
    } else {
      freq_[biggest] = static_cast<uint16_t>(freq_[biggest] + 1);
      ++sum;
    }
  }

  uint32_t acc = 0;
  for (uint32_t s = 0; s < alphabet; ++s) {
    cum_[s] = static_cast<uint16_t>(acc);
    if (freq_[s]) {
      if (acc + freq_[s] > kRansTotal) {
        // Unreachable if the repair above is correct; refuse to write rather
        // than corrupt the heap if it ever is not.
        std::fill(slot_sym_.begin(), slot_sym_.end(), uint8_t{0});
        freq_.assign(alphabet, 0);
        cum_.assign(alphabet, 0);
        freq_[0] = kRansTotal;
        return;
      }
      for (uint32_t k = 0; k < freq_[s]; ++k) {
        slot_sym_[acc + k] = static_cast<uint8_t>(s);
      }
      acc += freq_[s];
    }
  }
}

void RansTable::write(ByteWriter& w) const {
  uint32_t nonzero = 0;
  for (uint32_t s = 0; s < alphabet_; ++s) {
    if (freq_[s]) ++nonzero;
  }
  w.varint(nonzero);
  for (uint32_t s = 0; s < alphabet_; ++s) {
    if (freq_[s]) {
      w.varint(s);
      w.varint(freq_[s]);
    }
  }
}

Err RansTable::read(ByteReader& r) {
  uint64_t nonzero = 0;
  if (!r.varint(nonzero)) return Err::BadVarint;
  if (nonzero == 0 || nonzero > 256) return Err::BadMode;

  std::vector<uint32_t> counts(256, 0);
  uint64_t sum = 0;
  for (uint64_t i = 0; i < nonzero; ++i) {
    uint64_t sym = 0, freq = 0;
    if (!r.varint(sym)) return Err::BadVarint;
    if (!r.varint(freq)) return Err::BadVarint;
    if (sym >= 256) return Err::BadMode;
    if (freq == 0 || freq > kRansTotal) return Err::BadMode;
    counts[sym] = static_cast<uint32_t>(freq);
    sum += freq;
  }
  // A table whose frequencies do not fill the 12-bit space would desynchronise
  // the decoder; refuse rather than decode garbage.
  if (sum != kRansTotal) return Err::BadMode;

  alphabet_ = 256;
  freq_.assign(256, 0);
  cum_.assign(256, 0);
  slot_sym_.assign(kRansTotal, 0);
  uint32_t acc = 0;
  for (uint32_t s = 0; s < 256; ++s) {
    freq_[s] = static_cast<uint16_t>(counts[s]);
    cum_[s] = static_cast<uint16_t>(acc);
    for (uint32_t k = 0; k < counts[s]; ++k) {
      slot_sym_[acc + k] = static_cast<uint8_t>(s);
    }
    acc += counts[s];
  }
  return Err::Ok;
}

size_t RansTable::memory_bytes() const {
  return freq_.capacity() * 2 + cum_.capacity() * 2 + slot_sym_.capacity();
}

void RansEncoder::put(const RansTable& t, uint32_t sym, uint64_t index) {
  const uint32_t lane = single_lane_ ? 0 : static_cast<uint32_t>(index & (kRansLanes - 1));
  uint32_t& st = state_[lane];
  const uint32_t f = t.freq(sym);
  const uint32_t start = t.start(sym);
  const uint32_t x_max = ((kRansL >> kRansScaleBits) << 8) * f;
  while (st >= x_max) {
    buf_.push_back(static_cast<uint8_t>(st & 0xFF));
    st >>= 8;
  }
  st = ((st / f) << kRansScaleBits) + (st % f) + start;
}

std::vector<uint8_t> RansEncoder::finish() {
  // Pushed in reverse lane order so that after the reversal below the decoder
  // reads lane 0 first, then 1, 2, 3.
  for (uint32_t lane = kRansLanes; lane-- > 0;) {
    uint32_t st = state_[lane];
    for (int i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<uint8_t>(st & 0xFF));
      st >>= 8;
    }
  }
  std::reverse(buf_.begin(), buf_.end());
  return std::move(buf_);
}

RansDecoder::RansDecoder(std::span<const uint8_t> payload) : in_(payload) {
  if (in_.size() < 4) {
    ok_ = false;
    return;
  }
  for (uint32_t lane = 0; lane < kRansLanes; ++lane) {
    uint32_t st = 0;
    for (int i = 0; i < 4; ++i) st = (st << 8) | next();
    state_[lane] = st;
    if (st < kRansL) ok_ = false;
  }
}

uint8_t RansDecoder::next() {
  if (pos_ >= in_.size()) {
    ok_ = false;
    return 0;
  }
  return in_[pos_++];
}

uint32_t RansDecoder::get(const RansTable& t) {
  const uint32_t lane = single_lane_ ? 0 : (lane_next_++ & (kRansLanes - 1));
  uint32_t& st = state_[lane];
  const uint32_t slot = st & (kRansTotal - 1);
  const uint32_t sym = t.symbol_at(slot);
  const uint32_t f = t.freq(sym);
  const uint32_t start = t.start(sym);
  if (f == 0) {
    ok_ = false;
    return 0;
  }
  st = f * (st >> kRansScaleBits) + slot - start;
  // Renormalise until the state is back in range. This must be a loop, not a
  // single read: with a skewed table the encoder can emit several bytes at one
  // put, and the decoder has to consume all of them. (An earlier version read
  // exactly one byte here and desynced on highly skewed data.)
  while (st < kRansL) st = (st << 8) | next();
  return sym;
}

}  // namespace chce
