// brotli_wrap.hpp — a brotli stream inflated, with the stream reproducible.
//
// `.br` is what web assets ship as, and the engine treats it as opaque bytes: measured on a
// 512,000-byte tar, a `.tar.br` cost 149,606 bytes through the engine against 127,557 for the tar
// itself, a 14.7% gap (FINDINGS.md §3h).
//
// Two things make this harder than .xz, and both are worth stating before the code:
//
//   1. **Brotli has no magic number.** There is nothing to look for. The only test is "does it
//      decode", so `brotli_unpack` tries, and refuses when the decode fails or the stream does not
//      come back byte-identical.
//   2. **The quality is not in the stream.** The header carries the window size and nothing else,
//      so the producing quality has to be guessed from a shortlist and confirmed by re-encoding.
//
// The confirmation is what makes both problems safe: a payload that does not rebuild its own input
// byte for byte is discarded, so a wrong guess costs time and never data.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool brotli_available();

// Payload:
//   "BR01" | u8 quality | u8 mode | u8 lgwin | varint inflated_len | inflated bytes
//
// `quality` is 0-11, `mode` is 0 generic / 1 text / 2 font, `lgwin` is the window bits the encoder
// was given. All three are recovered by trying them and keeping the one that reproduces the stream.
bool brotli_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

// Rebuilds the original brotli stream from the payload.
bool brotli_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
