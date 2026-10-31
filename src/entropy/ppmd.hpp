// ppmd.hpp — PPMd backend, from the 7-Zip SDK (public domain).
//
// PPM models the *context* of each symbol rather than its position in a window,
// which is why it beats LZ and BWT on natural language and loses on data with no
// statistical structure. Offered per block, so text blocks take it and binary
// blocks do not.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

bool ppmd_available();

// Compresses one block. False when the result is not smaller.
bool ppmd_compress_block(std::span<const uint8_t> block, std::vector<uint8_t>& out);

// Decompresses a block produced above.
Err ppmd_decompress_block(std::span<const uint8_t> body, uint32_t out_size,
                          std::vector<uint8_t>& out);

}  // namespace chce
