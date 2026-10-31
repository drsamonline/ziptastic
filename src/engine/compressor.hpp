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
  // Phase 5 adaptive tables. NOT correct on multi-chunk inputs yet — see
  // docs/BENCHMARKS.md — so it is off and unreachable from the CLI.
  bool adaptive = false;
  uint32_t window = 64u << 20; // match-finder window
};

struct CompressStats {
  uint64_t blocks = 0;
  uint64_t raw_blocks = 0;
  uint64_t lzma2_blocks = 0;
  uint64_t parsed_blocks = 0;
  uint64_t tokens = 0;
  uint64_t payload_bytes = 0;
  // True when the LZMA2 fallback won and the stream is one fallback block.
  bool lzma2_stream = false;
};

// Compresses `input` into `out`. `out` is replaced, not appended to.
Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats = nullptr);

}  // namespace chce
