// coarse_index.cpp — block-hash index for long-range matches.
#include "chimera/coarse_index.hpp"

#include <algorithm>
#include <cstring>

namespace chce {
namespace {

uint32_t pow2_ceil(uint64_t v) {
  uint32_t p = 1;
  while (p < v && p < (1u << 31)) p <<= 1;
  return p;
}

}  // namespace

CoarseIndex::CoarseIndex(std::span<const uint8_t> data) : CoarseIndex(data, Options{}) {}

CoarseIndex::CoarseIndex(std::span<const uint8_t> data, const Options& opt)
    : data_(data), opt_(opt) {
  if (opt_.block_len < 8) opt_.block_len = 8;
  if (opt_.entries < 2) opt_.entries = 2;
  if (opt_.bucket_bits < 12) opt_.bucket_bits = 12;
  if (opt_.bucket_bits > 24) opt_.bucket_bits = 24;

  // Size the table to the input where that is affordable, and to the cap where
  // it is not. A 100 MiB input does not get a 100 MiB-sized table.
  const uint64_t want = std::max<uint64_t>(data.size() / 16, 1u << 12);
  uint32_t bits = 12;
  while (bits < opt_.bucket_bits && (1ull << bits) < want) ++bits;
  bucket_count_ = 1u << bits;
  mask_ = bucket_count_ - 1;

  anchored_ = opt_.entries / 2;
  slots_.assign(static_cast<size_t>(bucket_count_) * opt_.entries, 0);
  filled_.assign(bucket_count_, 0);
  rotating_.assign(bucket_count_, 0);
}

size_t CoarseIndex::memory_bytes() const {
  return slots_.capacity() * sizeof(uint32_t) +
         (filled_.capacity() + rotating_.capacity()) * sizeof(uint32_t);
}

uint32_t CoarseIndex::hash_at(uint32_t pos) const {
  // Hash the first 8 bytes with a multiplicative hash. Collisions are expected
  // and harmless: every candidate is verified byte for byte before it is
  // reported, so a collision costs a comparison, not a wrong match.
  uint64_t v = 0;
  std::memcpy(&v, data_.data() + pos, sizeof(v));
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33;
  return static_cast<uint32_t>(v) & mask_;
}

void CoarseIndex::insert(uint32_t pos) {
  if (pos + opt_.block_len > data_.size()) return;

  const uint32_t b = hash_at(pos);
  uint32_t* base = &slots_[static_cast<size_t>(b) * opt_.entries];

  // Anchored half: fill once, keep forever. This is what survives a long run of
  // incompressible data between two copies of the same content.
  if (filled_[b] < anchored_) {
    base[filled_[b]] = pos + 1;  // 0 means "empty"
    ++filled_[b];
    return;
  }
  // Rotating half: most recent wins.
  const uint32_t slot = anchored_ + (rotating_[b] % (opt_.entries - anchored_));
  base[slot] = pos + 1;
  ++rotating_[b];
}

bool CoarseIndex::query(uint32_t pos, Match* out) const {
  if (pos + opt_.block_len > data_.size()) return false;

  const uint32_t b = hash_at(pos);
  const uint32_t* base = &slots_[static_cast<size_t>(b) * opt_.entries];

  const uint32_t limit =
      static_cast<uint32_t>(std::min<uint64_t>(opt_.max_len, data_.size() - pos));

  uint32_t best_len = 0;
  uint32_t best_dist = 0;

  for (uint32_t i = 0; i < opt_.entries; ++i) {
    const uint32_t stored = base[i];
    if (stored == 0) continue;  // empty
    const uint32_t cand = stored - 1;
    if (cand >= pos) continue;  // only backwards
    const uint32_t dist = pos - cand;
    if (dist < 1) continue;

    // Cheap reject: the first bytes must agree. The hash already suggests they
    // do, but a collision must not become a false match.
    const uint8_t* a = data_.data() + pos;
    const uint8_t* c = data_.data() + cand;
    if (a[0] != c[0] || a[opt_.block_len - 1] != c[opt_.block_len - 1]) continue;

    uint32_t len = 0;
    while (len < limit && a[len] == c[len]) ++len;
    if (len < opt_.block_len) continue;  // too short to be worth the distance

    if (len > best_len) {
      best_len = len;
      best_dist = dist;
    }
  }

  if (best_len == 0) return false;
  out->len = best_len;
  out->dist = best_dist;
  return true;
}

}  // namespace chce
