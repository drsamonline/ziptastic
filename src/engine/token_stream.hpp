// token_stream.hpp — serialises a parse into a block body, and back.
//
// This is the transitional Phase 3 encoding: literal runs go out as raw bytes,
// matches as a length and a distance. There is no entropy coding here at all,
// which is exactly why the ratio is poor and why Phase 4 replaces this body
// with rANS while keeping the parse unchanged.
//
// Body layout (docs/FORMAT.md, mode LZ_RAW):
//   repeated until the block's uncompressed_size is produced:
//     0x00  varint count  count bytes      literal run
//     0x01  varint len    varint dist      match
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"
#include "parser.hpp"

namespace chce {

// Appends the encoded token stream for `tokens` (which describe
// `data[begin, begin + produced)`) to `w`.
void encode_tokens(ByteWriter& w, std::span<const uint8_t> data, uint32_t begin,
                   const std::vector<Token>& tokens);

// Decodes a token-stream body, appending exactly `out_size` bytes to `out`.
// Matches may reference bytes produced by *earlier* blocks: the LZ window
// carries across the stream, so `out` is the whole output so far, not just
// this block.
Err decode_tokens(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out);

}  // namespace chce
