// bwt_bsc.hpp — the BWT backend, via libbsc.
//
// Burrows-Wheeler is a different family from LZ: it sorts the block, then codes
// runs. It wins on data whose structure is statistical rather than repeated —
// binary records, some image and table data — and loses on ordinary text, where
// the LZ + range path is stronger. Offering it as a per-block candidate means we
// only pay for it where it wins.
//
// Vendored source: vendor/hcs/old/libbsc-master (Apache-2.0, verified).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

bool bsc_available();

// Compresses one block. Returns false when libbsc declines (its own
// "not compressible" answer) or when the result is not smaller.
bool bsc_compress_block(std::span<const uint8_t> block, std::vector<uint8_t>& out);

// Decompresses a block previously produced by bsc_compress_block.
Err bsc_decompress_block(std::span<const uint8_t> body, uint32_t out_size,
                         std::vector<uint8_t>& out);

}  // namespace chce
