// bmp_wrap.hpp — BMP pixel data split into colour planes, delta-coded per row.
//
// An uncompressed BMP is raw pixels in BGR order, row by row. Two things make that a
// poor shape for a compressor: the three colour channels are interleaved, so a match has
// to step three bytes at a time, and neighbouring pixels differ by a little rather than
// being equal.
//
// Measured (docs/BMP_MEASUREMENT.md), post-compression:
//
//   512x512 photographic-ish, 786,486 B BMP   127,319 -> 1,395   -98.9%
//   256x256 sprite sheet,   196,662 B BMP         365 ->   640   +75%  (declined)
//
// So the transform is gated by a trial, like precomp and PNG: a flat, tiled image is
// already trivial to compress, and splitting its planes destroys the row structure the
// backend was using.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace chce {

bool bmp_available();

// "BM" plus a BITMAPINFOHEADER with uncompressed 24- or 32-bit pixels.
bool bmp_likely(std::span<const uint8_t> in);

// Payload: varint header_len | varint width | varint height | varint channels |
// header bytes | one delta-coded plane per channel, rows in file order.
bool bmp_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed);
bool bmp_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out);

}  // namespace chce
