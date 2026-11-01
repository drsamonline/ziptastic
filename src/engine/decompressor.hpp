// decompressor.hpp — the CHCE decode path.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

// Sentinel for "the failure was not attributable to one block".
inline constexpr uint64_t kNoBlock = ~0ull;

struct DecompressReport {
  uint64_t blocks = 0;
  uint64_t original_size = 0;
  // Which block failed its integrity check, when one did. Naming the block is
  // the point of per-block digests: with only a whole-file CRC-32 the caller
  // learns that something is wrong, never what.
  uint64_t bad_block = kNoBlock;
};

// Decodes a whole CHCE v1 stream. Returns Err::Ok only if the output is
// exactly `header.original_size` bytes and the stream was fully consumed.
// `out` is replaced, not appended to.
Err decompress(std::span<const uint8_t> in, std::vector<uint8_t>& out,
               DecompressReport* report = nullptr);

}  // namespace chce
