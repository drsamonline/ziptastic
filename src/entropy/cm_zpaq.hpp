// cm_zpaq.hpp — context-mixing backend (libzpaq) for --max-ratio.
//
// This is the ratio ceiling: a context-mixing compressor in the PAQ family. It is
// far slower than anything else in the engine — tens of times slower than the LZ
// path — so it is only offered when the caller explicitly asks for --max-ratio.
// Without that flag it is never constructed.
//
// Vendored source: vendor/hcs/old/zpaq-master (public domain).
#pragma once

#include <cstdint>
#include <vector>

// Deliberately free of chimera/format.hpp: this file is compiled as C++17 (it
// includes libzpaq.h, which predates C++20) and format.hpp uses std::span.

namespace chce {

bool zpaq_available();

// Compresses one block with context mixing. Returns false when the result would
// not be smaller.
bool zpaq_compress_block(const std::vector<uint8_t>& block, std::vector<uint8_t>& out);

// Decompresses a block produced by zpaq_compress_block. False means the payload
// did not decode to exactly `out_size` bytes.
bool zpaq_decompress_block(const std::vector<uint8_t>& body, uint32_t out_size,
                           std::vector<uint8_t>& out);

}  // namespace chce
