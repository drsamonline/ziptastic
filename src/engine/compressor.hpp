// compressor.hpp — the CHCE block loop.
//
// Phase 0: one candidate encoder (RAW). Phase 1 adds LZMA2 to the candidate
// set, Phase 4+ adds the rANS backends, and the mode selector chooses the
// smallest. The loop shape below does not change across those phases: read a
// block, encode candidates, pick the smallest, append the block, carry state.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/block.hpp"
#include "chimera/format.hpp"

namespace chce {

struct CompressOptions {
  size_t block_size = kDefaultBlockSize;
  bool max_ratio = false;  // --max-ratio: allow CM_ZPAQ candidates
  bool precomp = false;    // Layer 0 semantic precompression
  bool coarse = false;     // consult the coarse (SREP-style) index
};

struct CompressStats {
  uint64_t blocks = 0;
  uint64_t raw_blocks = 0;
  uint64_t payload_bytes = 0;
};

// Compresses `input` into `out`. `out` is replaced, not appended to.
Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats = nullptr);

}  // namespace chce
