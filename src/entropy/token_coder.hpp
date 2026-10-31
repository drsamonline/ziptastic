// token_coder.hpp — RANS_CTX: the parse, coded with context-modelled rANS.
//
// Symbols and their contexts (docs/FORMAT.md, mode RANS_CTX):
//   is_match   ctx = position & 3                            2 symbols
//   literal    ctx = previous byte >> 4                     256 symbols
//   len slot   ctx = position & 3                            32 symbols
//   dist slot  ctx = min(3, len_slot >> 2)                   64 symbols
//   extra bits (length and distance remainders)              uniform, 1 bit each
//
// Tables are measured on the block and written into the block body, so this is
// a static coder: Phase 5 replaces the measurement with adaptation.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"
#include "engine/parser.hpp"

namespace chce {

// Length: 2..9 map to slots 0..7; longer lengths take a slot plus extra bits.
uint32_t len_slot(uint32_t len);
uint32_t len_base(uint32_t slot);
uint32_t len_extra_bits(uint32_t slot);

// Distance: the three nearest take slots 1..3; the rest use LZMA's pairing.
uint32_t dist_slot(uint32_t dist);
uint32_t dist_base(uint32_t slot);
uint32_t dist_extra_bits(uint32_t slot);

// Encodes `tokens` (describing data[begin, end)) into `w`.
void encode_tokens_rans(ByteWriter& w, std::span<const uint8_t> data,
                        uint32_t begin, uint32_t end,
                        const std::vector<Token>& tokens);

// Encodes with the chunked adaptive model: chunk 0 carries measured tables and
// every later chunk is coded with a table derived from the symbols already
// decoded. Nothing after chunk 0 is transmitted.
void encode_tokens_rans_adaptive(ByteWriter& w, std::span<const uint8_t> data,
                                 uint32_t begin, uint32_t end,
                                 const std::vector<Token>& tokens);

// Decodes a RANS_CTX body, appending exactly `out_size` bytes to `out`.
// Matches may reference bytes from earlier blocks: the window carries.
Err decode_tokens_rans(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out);

}  // namespace chce
