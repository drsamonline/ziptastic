// huffman.hpp — static (two-pass) and adaptive Huffman, coding block bytes.
//
// These are byte-level entropy backends, not token backends. That is a
// deliberate choice: a Huffman coder over the *token* stream would be strictly
// worse than the rANS and range coders already in the tree and would never be
// selected, so it would be dead weight. Coding the block's bytes instead gives
// a genuinely different trade-off — it helps where LZ finds little but the byte
// distribution is skewed (paletted images, text with a small alphabet) and loses
// to RAW where there is no skew at all.
//
// HUFF_STATIC sends the code lengths in the body: 256 bytes, which is 0.1% of a
// 256 KiB block.
// HUFF_ADAPT sends nothing: both sides start from the same flat tree and rebuild
// it every kRebuild symbols from counts they each maintain, so the coder adapts
// without transmitting anything.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

// Longest code we will assign. A deeper tree is possible for pathological
// distributions; when it happens both sides fall back to flat 8-bit codes, which
// is deterministic and still correct.
inline constexpr uint32_t kMaxCodeLen = 24;
inline constexpr uint32_t kAdaptiveRebuild = 256;

struct HuffCodes {
  uint8_t len[256] = {};
  uint32_t code[256] = {};
};

// Builds canonical codes from symbol frequencies. Deterministic: ties are broken
// by symbol value and then by node creation order, so the encoder and decoder
// always produce the same tree from the same counts.
void huff_build(const uint32_t freq[256], HuffCodes& out);

// Static: body = 256 code lengths, then the bitstream.
void huff_static_encode(std::span<const uint8_t> block, ByteWriter& w);
Err huff_static_decode(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out);

// Adaptive: no header at all; the tree evolves as symbols are coded.
void huff_adaptive_encode(std::span<const uint8_t> block, ByteWriter& w);
Err huff_adaptive_decode(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out);

}  // namespace chce
