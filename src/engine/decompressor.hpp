// decompressor.hpp — the CHCE decode path.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

struct DecompressReport {
  uint64_t blocks = 0;
  uint64_t original_size = 0;
};

// Decodes a whole CHCE v1 stream. Returns Err::Ok only if the output is
// exactly `header.original_size` bytes and the stream was fully consumed.
// `out` is replaced, not appended to.
Err decompress(std::span<const uint8_t> in, std::vector<uint8_t>& out,
               DecompressReport* report = nullptr);

}  // namespace chce
