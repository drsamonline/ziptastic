// gif_wrap.hpp — GIF recompression as a Layer 0 transform.
//
// A GIF is LZW-compressed palette indices. LZW is a 1980s coder and loses badly
// to what this engine has: measured on a dithered photo-like image, compressing
// the decoded indices instead of the LZW stream is worth -22.1%.
//
// The transform is only used when the original LZW stream can be reproduced
// BYTE-EXACTLY from the indices. A GIF's LZW is not canonical — clear codes and
// code-width changes are encoder choices — so a re-encode that differs by one bit
// disqualifies the file. Verification happens before the transform is committed,
// exactly as for preflate, dedup and JPEG.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool gif_available();

// Cheap check for the GIF signature.
bool gif_likely(std::span<const uint8_t> in);

// Decodes every image's LZW to indices and builds a payload of
// [structure with LZW removed] + [indices]. False when the file is not a GIF,
// or when the LZW cannot be reproduced exactly.
bool gif_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);

// Rebuilds the original GIF bytes.
bool gif_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
