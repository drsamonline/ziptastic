// match_finder.cpp — BT4 binary-tree match finder.
//
// Ported from the LZMA SDK's Bt4_MatchFinder_GetMatches / SkipMatchesSpec
// (vendor/hcs/old/lzma/C/LzFind.c, public domain). The algorithm is
// unchanged; what is new is the container (RAII, bounds-checked spans) and
// the contract: matches come back ordered by increasing length, distances are
// real byte distances, and the tree is rebuilt deterministically for a given
// input so the whole engine stays byte-for-byte reproducible.
#include "chimera/match_finder.hpp"

#include <algorithm>
#include <cstring>

namespace chce {
namespace {

// Round up to a power of two, so the cyclic buffer can be indexed with a mask.
uint32_t pow2_ceil(uint64_t v) {
  uint32_t p = 1;
  while (p < v && p < (1u << 31)) p <<= 1;
  return p;
}

}  // namespace

MatchFinder::MatchFinder(std::span<const uint8_t> data)
    : MatchFinder(data, Options{}) {}

MatchFinder::MatchFinder(std::span<const uint8_t> data, const Options& opt)
    : data_(data), opt_(opt) {
  // The index stores positions as uint32, so inputs beyond 4 GiB - 2 cannot be
  // addressed. Refuse rather than wrap: a silent wrap would produce matches
  // that point at the wrong bytes.
  const uint64_t usable =
      std::min<uint64_t>(data.size(), static_cast<uint64_t>(0xFFFFFFFEu));

  // LZMA sizes its cyclic buffer to min(dictionary, file size). Doing the same
  // means a 100 KB file does not pay for a 64 MiB window.
  uint32_t window = opt_.window;
  window = std::min<uint32_t>(window, 64u << 20);
  window = std::min<uint32_t>(window, static_cast<uint32_t>(std::max<uint64_t>(usable, 1u)));
  window_ = pow2_ceil(std::max<uint32_t>(window, 2));
  mask_ = window_ - 1;

  depth_limit_ = opt_.depth ? opt_.depth : (16u + opt_.nice_len / 2);
  if (opt_.nice_len < 4) opt_.nice_len = 4;
  if (opt_.hash_bits < 16) opt_.hash_bits = 16;
  if (opt_.hash_bits > 26) opt_.hash_bits = 26;

  // A fixed 2^22 table costs 16 MiB whether the input is 100 KiB or 100 MiB.
  // Scale it to roughly one bucket per two input bytes, never above the
  // requested size: small files get small tables, large files get the cap.
  uint32_t bits = 16;
  while (bits < opt_.hash_bits && (1ull << bits) < (usable / 2)) ++bits;
  hash_bits_ = bits;
  hash_mask_ = (1u << hash_bits_) - 1;

  hash2_.assign(1u << 16, kEmpty);
  hash3_.assign(1u << 16, kEmpty);
  hash4_.assign(static_cast<size_t>(hash_mask_) + 1, kEmpty);
  son_.assign(static_cast<size_t>(window_) * 2, kEmpty);
  matches_.reserve(opt_.max_matches);
}

size_t MatchFinder::memory_bytes() const {
  return hash2_.size() * sizeof(uint32_t) + hash3_.size() * sizeof(uint32_t) +
         hash4_.size() * sizeof(uint32_t) + son_.size() * sizeof(uint32_t) +
         matches_.capacity() * sizeof(Match);
}

uint32_t MatchFinder::hash4(uint32_t pos) const {
  uint32_t v = 0;
  std::memcpy(&v, data_.data() + pos, sizeof(v));
  // Multiplicative hash; the top bits of the product are the well-mixed ones.
  return (v * 2654435761u) >> (32 - hash_bits_);
}

void MatchFinder::add_match(uint32_t len, uint32_t dist) {
  // The contract is increasing length, and a longer match found later is the
  // only interesting one: shorter duplicates carry no new information.
  if (len <= best_len_) return;
  if (matches_.size() >= opt_.max_matches) return;
  best_len_ = len;
  matches_.push_back(Match{len, dist});
}

void MatchFinder::insert(uint32_t pos, bool collect) {
  const uint32_t len_limit = static_cast<uint32_t>(
      std::min<uint64_t>(opt_.nice_len, data_.size() - pos));
  if (len_limit < 2) return;

  const uint8_t* cur = data_.data() + pos;
  // A distance of exactly `window_` maps the candidate onto *this* position's
  // own tree slot (the cyclic buffer is indexed pos & mask), so it must be
  // excluded. Anything closer is distinct and legal — including a candidate
  // at position 0 before the buffer has filled, which is why this is checked
  // against window_ and not against min(pos, window_).
  const uint32_t max_dist = window_;

  // --- 2-byte and 3-byte prefixes -----------------------------------------
  const uint32_t h2 = static_cast<uint32_t>(cur[0]) |
                      (static_cast<uint32_t>(cur[1]) << 8);
  const uint32_t d2 = hash2_[h2] == kEmpty ? kEmpty : pos - hash2_[h2];
  hash2_[h2] = pos;

  uint32_t d3 = kEmpty;
  if (len_limit >= 3) {
    const uint32_t h3 = (static_cast<uint32_t>(cur[0]) |
                         (static_cast<uint32_t>(cur[1]) << 8) |
                         (static_cast<uint32_t>(cur[2]) << 16)) & 0xFFFFu;
    d3 = hash3_[h3] == kEmpty ? kEmpty : pos - hash3_[h3];
    hash3_[h3] = pos;
  }

  uint32_t dist_cap = 0;  // distance of the short-prefix candidate

  if (collect) {
    // A 2-byte candidate, then a 3-byte one if the 2-byte one does not already
    // reach three bytes — the same order and the same reasoning as LZMA.
    bool have = false;
    if (d2 != kEmpty && d2 < max_dist && cur[-static_cast<ptrdiff_t>(d2)] == cur[0]) {
      dist_cap = d2;
      have = true;
      if (len_limit > 2 && cur[2] != cur[-static_cast<ptrdiff_t>(d2) + 2]) {
        if (d3 != kEmpty && d3 < max_dist && cur[-static_cast<ptrdiff_t>(d3)] == cur[0]) {
          dist_cap = d3;
        }
      }
    } else if (d3 != kEmpty && d3 < max_dist && cur[-static_cast<ptrdiff_t>(d3)] == cur[0]) {
      dist_cap = d3;
      have = true;
    }
    if (have) {
      // Extend the short candidate as far as it goes; it may be long.
      uint32_t len = 2;
      const uint8_t* other = cur - dist_cap;
      while (len < len_limit && other[len] == cur[len]) ++len;
      add_match(len, dist_cap);
      // Do NOT short-circuit when the short candidate already reaches
      // len_limit. An earlier version grafted the 4-byte hash head's subtree
      // here, but that head is a different position and nothing has verified
      // that it matches this far — the graft corrupted the tree's ordering
      // invariant and the walk later reported matches that do not exist. The
      // tree walk below handles this case correctly on its own: add_match
      // ignores anything not longer than len_limit, and the walk's own
      // len == len_limit branch grafts from a candidate it has just compared.
    }
  }

  if (len_limit < 4) {
    ++inserted_;
    return;
  }

  // --- 4-byte hash and the binary tree ------------------------------------
  const uint32_t h = hash4(pos);
  uint32_t cur_match = hash4_[h];
  hash4_[h] = pos;

  uint32_t* ptr0 = &son_[2 * (pos & mask_)];
  uint32_t* ptr1 = &son_[2 * (pos & mask_) + 1];
  uint32_t len0 = 0;
  uint32_t len1 = 0;
  uint32_t depth = depth_limit_;

  for (;;) {
    if (cur_match == kEmpty) {
      *ptr0 = kEmpty;
      *ptr1 = kEmpty;
      break;
    }
    const uint32_t dist = pos - cur_match;
    if (dist >= max_dist) {
      *ptr0 = kEmpty;
      *ptr1 = kEmpty;
      break;
    }

    const uint8_t* other = data_.data() + cur_match;
    uint32_t len = std::min(len0, len1);
    while (len < len_limit && other[len] == cur[len]) ++len;

    if (collect) add_match(len, dist);

    if (len == len_limit) {
      // This match already covers everything we would accept; take over both
      // of the old node's children and stop.
      const uint32_t* other_slot = &son_[2 * (cur_match & mask_)];
      *ptr0 = other_slot[0];
      *ptr1 = other_slot[1];
      break;
    }
    if (--depth == 0) {
      // Depth exhausted: stop here and leave no children. LZMA does exactly
      // this (*ptr0 = *ptr1 = kEmptyHashValue). Adopting the candidate's
      // subtree instead — as an earlier version of this file did — is only
      // sound when the match reached len_limit, because only then does this
      // position occupy the same place in the ordering. With len < len_limit
      // the children can sit on the wrong side of us, which breaks the
      // "shares min(len0, len1) bytes" invariant the walk relies on and
      // makes it report matches that do not exist.
      *ptr0 = kEmpty;
      *ptr1 = kEmpty;
      break;
    }

    if (other[len] < cur[len]) {
      // Candidate sorts before us: it becomes our left child.
      *ptr0 = cur_match;
      ptr0 = &son_[2 * (cur_match & mask_) + 1];
      cur_match = *ptr0;
      len0 = len;
    } else {
      *ptr1 = cur_match;
      ptr1 = &son_[2 * (cur_match & mask_)];
      cur_match = *ptr1;
      len1 = len;
    }
  }

  ++inserted_;
}

std::span<const Match> MatchFinder::query(uint32_t pos) {
  matches_.clear();
  best_len_ = 0;

  if (pos >= data_.size()) {
    next_ = static_cast<uint32_t>(data_.size());
    return {};
  }
  // Never re-insert a position: the tree would get a cycle through itself.
  if (pos < next_) return {};

  while (next_ < pos) {
    insert(next_, false);
    ++next_;
  }
  insert(pos, true);
  next_ = pos + 1;
  return matches_;
}

void MatchFinder::skip_to(uint32_t pos) {
  if (pos > data_.size()) pos = static_cast<uint32_t>(data_.size());
  while (next_ < pos) {
    insert(next_, false);
    ++next_;
  }
}

}  // namespace chce
