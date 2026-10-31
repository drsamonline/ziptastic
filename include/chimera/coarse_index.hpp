// coarse_index.hpp — the coarse (long-range) match index.
//
// The fine finder has a 64 MiB window: a repeat 100 MiB back is invisible to it.
// This index hashes fixed-size blocks and keeps recent positions per bucket, so
// a repeat anywhere earlier in the stream can still be found, at the cost of a
// longer distance code.
//
// Written from scratch. The design follows the published description of
// block-hash long-range matching (SREP's approach), but no SREP code is used:
// its licence is all-rights-reserved, as recorded in the HCS manifest and in
// docs/SOURCES_REVIEW.md.
//
// Bucket policy, which matters more than it looks: half of each bucket is
// "anchored" — filled once, never overwritten — and half rotates. Pure
// round-robin evicts early positions under a long stream of noise, which is
// exactly the case this index exists to handle (a file repeated after a large
// incompressible gap). Pure first-come-keeps would lose locality on normal
// data. Half and half gets both.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/match_finder.hpp"

namespace chce {

class CoarseIndex {
 public:
  struct Options {
    // Bytes that must match before a candidate is reported. Long-range matches
    // are only worth their distance code if they are long.
    uint32_t block_len = 48;
    // log2 of the bucket count. 21 = 2 Mi buckets.
    uint32_t bucket_bits = 21;
    // Positions kept per bucket, split half anchored / half rotating.
    uint32_t entries = 16;
    // Longest match reported; matches the token format's limit.
    uint32_t max_len = 273;
  };

  // `data` must outlive the index.
  explicit CoarseIndex(std::span<const uint8_t> data);
  CoarseIndex(std::span<const uint8_t> data, const Options& opt);

  // Records the position. Must be called with increasing positions; positions
  // are inserted once each, as the parser walks forward.
  void insert(uint32_t pos);

  // Looks for the longest coarse match at `pos`. Returns false when there is
  // none of at least `block_len` bytes.
  bool query(uint32_t pos, Match* out) const;

  size_t memory_bytes() const;
  uint32_t buckets() const { return bucket_count_; }

 private:
  uint32_t hash_at(uint32_t pos) const;

  std::span<const uint8_t> data_;
  Options opt_;
  uint32_t bucket_count_ = 0;
  uint32_t mask_ = 0;
  uint32_t anchored_ = 0;  // slots per bucket that are never overwritten
  std::vector<uint32_t> slots_;  // bucket_count_ * entries
  std::vector<uint32_t> filled_;  // per bucket: anchored slots used
  std::vector<uint32_t> rotating_;  // per bucket: round-robin cursor
};

}  // namespace chce
