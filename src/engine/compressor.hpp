// compressor.hpp — the CHCE block loop.
//
// Phase 0: one candidate encoder (RAW). Phase 1 adds the whole-stream LZMA2
// fallback, which is chosen as an alternative to the entire CHCE-native
// encoding rather than per block (see src/codecs/lzma2_fallback.hpp for why).
// Phase 4+ adds per-block entropy backends, and the mode selector then picks
// the smallest per block. The loop shape below does not change across those
// phases: read a block, encode candidates, pick the smallest, append, carry
// state.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/block.hpp"
#include "chimera/format.hpp"

namespace chce {

struct CompressOptions {
  size_t block_size = kDefaultBlockSize;
  bool max_ratio = false;      // --max-ratio: allow CM_ZPAQ candidates
  bool precomp = false;        // Layer 0 semantic precompression
  bool coarse = false;         // consult the coarse (SREP-style) index
  bool lzma2_fallback = true;  // Tier-1 guarantee; --no-lzma2 disables it
  bool parse = true;           // Phase 3 optimal parse (rANS candidate)
  // Phase 5 adaptive tables, offered as an extra per-block candidate. The mode
  // selector keeps whichever of static and adaptive is smaller, so enabling it
  // cannot make a block worse.
  bool adaptive = true;
  // Range-coded model: per-symbol adaptation, no tables. Wins on every corpus
  // file measured; costs encode time, so Phase 12 may gate it.
  bool range = true;
  // x86 BCJ transform, applied when the input looks like code.
  bool bcj = true;
  // Byte-level Huffman backends (Phase 7).
  bool huffman = true;
  // BWT backend (Phase 9).
  bool bwt = true;
  // Parallel mode: blocks become self-contained (no cross-block matches) so they
  // can be compressed and decompressed on several threads. Archive mode, the
  // default, keeps them coupled for ratio.
  // Coarse dedup stage (SREP-style): remove long-range duplicates before block
  // splitting, so backends that cannot look across blocks still benefit.
  bool dedup = true;
  bool parallel = false;
  unsigned threads = 0;  // 0 = hardware concurrency
  uint32_t window = 64u << 20; // match-finder window
};

struct CompressStats {
  uint64_t blocks = 0;
  uint64_t raw_blocks = 0;
  uint64_t lzma2_blocks = 0;
  uint64_t parsed_blocks = 0;
  uint64_t huff_blocks = 0;
  uint64_t bwt_blocks = 0;
  // Blocks per mode byte. The named counters above are kept for the CLI, but
  // this array is the authoritative one: adding a backend cannot make it stale,
  // which the named counters repeatedly did.
  uint64_t mode_counts[16] = {};
  uint64_t tokens = 0;
  uint64_t payload_bytes = 0;
  // True when the LZMA2 fallback won and the stream is one fallback block.
  bool lzma2_stream = false;
};

// Compresses `input` into `out`. `out` is replaced, not appended to.
Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats = nullptr);

}  // namespace chce
