// decompressor.cpp — block loop for decode.
//
// The decoder trusts nothing in the stream: every size is validated against
// what is actually present, and the total output is capped by the declared
// original_size so a fuzzed stream cannot force an unbounded allocation.
#include "decompressor.hpp"

#include <algorithm>

#include "chimera/block.hpp"

namespace chce {
namespace {

// Decodes one payload into `dst`. Phase 0 handles RAW; later phases add one
// branch per mode here.
Err decode_payload(const BlockHeader& hdr, std::span<const uint8_t> payload,
                   std::vector<uint8_t>& dst) {
  switch (hdr.mode) {
    case Mode::Raw:
      if (hdr.compressed_size != hdr.uncompressed_size) return Err::SizeMismatch;
      dst.insert(dst.end(), payload.begin(), payload.end());
      return Err::Ok;

    // Defined in the format but not yet implemented in this engine build.
    case Mode::RansCtx:
    case Mode::HuffStatic:
    case Mode::HuffAdapt:
    case Mode::BwtBsc:
    case Mode::Lzma2Fallback:
    case Mode::Brotli:
    case Mode::Zstd:
    case Mode::Lz4:
    case Mode::CmZpaq:
    case Mode::Ppmd:
      return Err::BadMode;
  }
  return Err::BadMode;
}

}  // namespace

Err decompress(std::span<const uint8_t> in, std::vector<uint8_t>& out,
               DecompressReport* report) {
  ByteReader r(in);

  FileHeader fh;
  if (Err e = read_file_header(r, fh); e != Err::Ok) return e;

  std::vector<uint8_t> dst;
  // Reserve at most 64 MiB up front; the rest grows as blocks are verified.
  dst.reserve(static_cast<size_t>(std::min<uint64_t>(fh.original_size, 64ull << 20)));

  uint64_t blocks = 0;
  while (blocks < fh.block_count) {
    BlockHeader bh;
    if (Err e = read_block_header(r, bh); e != Err::Ok) return e;

    // Never let a block claim more output than the stream declared.
    const uint64_t produced = dst.size();
    if (bh.uncompressed_size > fh.original_size - produced) {
      return Err::SizeMismatch;
    }

    std::span<const uint8_t> payload;
    if (!r.take(static_cast<size_t>(bh.compressed_size), payload)) {
      return Err::Truncated;
    }
    const size_t before = dst.size();
    if (Err e = decode_payload(bh, payload, dst); e != Err::Ok) return e;
    if (dst.size() - before != bh.uncompressed_size) return Err::SizeMismatch;

    ++blocks;
  }

  if (!r.at_end()) return Err::TrailingData;
  if (dst.size() != fh.original_size) return Err::SizeMismatch;

  out = std::move(dst);
  if (report) {
    report->blocks = blocks;
    report->original_size = fh.original_size;
  }
  return Err::Ok;
}

}  // namespace chce
