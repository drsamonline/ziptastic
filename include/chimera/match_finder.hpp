// match_finder.hpp — the fine match finder: a binary-tree (BT4) index.
//
// This is a C++20 port of the binary-tree match finder from the LZMA SDK
// (vendor/hcs/old/lzma/C/LzFind.c, public domain — `copy-src` in SOURCES).
// The structure is the same one LZMA uses: three hash tables (2-, 3- and
// 4-byte) feeding a binary search tree over positions, with a cyclic buffer
// of `window` slots so memory is bounded no matter how long the input is.
//
// The parser asks one question — "what matches are available at this
// position?" — and gets back a list ordered by increasing length, where each
// entry carries the *closest* distance at which that length was reached. That
// ordering is what lets a cost model price the list in one pass.
//
// Phase 6 will add a second, coarse index (SREP-style block hashing) beside
// this one; the parser will query both and the cost model will discount
// coarse matches on distance. Nothing here changes for that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace chce {

// One candidate match. `dist` is a real backward distance in bytes (1 means
// "one byte back"); the encoder subtracts 1 when it writes it, as LZMA does.
struct Match {
  uint32_t len;
  uint32_t dist;
};

class MatchFinder {
 public:
  struct Options {
    // Maximum backward distance. Rounded down to what the input can use and
    // capped at 64 MiB (the LZMA -9 dictionary size).
    uint32_t window = 64u << 20;
    // Stop extending once a match reaches this length; longer costs the
    // parser more than it saves.
    uint32_t nice_len = 64;
    // Tree-walk cutoff. 0 selects the LZMA default (16 + nice_len / 2).
    uint32_t depth = 0;
    // log2 of the 4-byte hash table size. 22 = 4 Mi slots = 16 MiB.
    uint32_t hash_bits = 22;
    // Hard cap on matches returned per position.
    uint32_t max_matches = 128;
  };

  // `data` must outlive the finder.
  explicit MatchFinder(std::span<const uint8_t> data);
  MatchFinder(std::span<const uint8_t> data, const Options& opt);

  // Returns the matches at `pos`, ordered by increasing length.
  //
  // Positions must be queried in increasing order: the index is built as we
  // walk forward, and a position is inserted exactly once. Querying a
  // position at or behind one already queried returns an empty list rather
  // than corrupting the tree.
  std::span<const Match> query(uint32_t pos);

  // Inserts everything up to `pos` without collecting matches, so the parser
  // can resume after a long match.
  void skip_to(uint32_t pos);

  uint32_t window_size() const { return window_; }
  uint32_t next_position() const { return next_; }
  size_t memory_bytes() const;
  uint64_t positions_inserted() const { return inserted_; }

  // Empty distance marker for the internal tables.
  static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

 private:
  void insert(uint32_t pos, bool collect);
  uint32_t hash4(uint32_t pos) const;
  void add_match(uint32_t len, uint32_t dist);

  std::span<const uint8_t> data_;
  Options opt_;
  uint32_t window_ = 0;   // power of two, >= 1
  uint32_t mask_ = 0;     // window_ - 1
  uint32_t depth_limit_ = 0;
  uint32_t hash_bits_ = 0;   // effective (scaled to the input)
  uint32_t hash_mask_ = 0;

  std::vector<uint32_t> hash2_;  // 2-byte prefix -> last position
  std::vector<uint32_t> hash3_;  // 3-byte prefix -> last position
  std::vector<uint32_t> hash4_;  // 4-byte prefix -> tree root
  std::vector<uint32_t> son_;    // 2 slots per position: left, right

  std::vector<Match> matches_;
  uint32_t next_ = 0;
  uint32_t best_len_ = 0;
  uint64_t inserted_ = 0;
};

}  // namespace chce
