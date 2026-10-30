// compressor.cpp — orchestrates the per-block encode loop.
#include "compressor.hpp"

#include <algorithm>
#include <cstring>

namespace chce {
namespace {

// One fully encoded block: header plus payload, ready to serialise.
struct EncodedBlock {
  BlockHeader hdr;
  std::vector<uint8_t> payload;
};

// RAW candidate: the block verbatim, no entropy coding.
EncodedBlock encode_raw(std::span<const uint8_t> block) {
  EncodedBlock b;
  b.hdr.mode = Mode::Raw;
  b.hdr.uncompressed_size = block.size();
  b.hdr.compressed_size = block.size();
  b.payload.assign(block.begin(), block.end());
  return b;
}

// Phase 1+ candidate set. Each additional backend appends a candidate here;
// the mode selector then picks the smallest payload. Kept as one function so
// the selection rule stays in a single place across phases.
std::vector<EncodedBlock> encode_candidates(std::span<const uint8_t> block,
                                            const CompressOptions& opt) {
  (void)opt;
  std::vector<EncodedBlock> cands;
  cands.push_back(encode_raw(block));
  return cands;
}

}  // namespace

Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats) {
  if (opt.block_size == 0 || opt.block_size > kMaxBlockSize) {
    return Err::BadBlockSize;
  }
  if (input.size() > kMaxStreamSize) return Err::BadBlockSize;

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = 0;
  if (opt.coarse) fh.flags |= kFlagCoarse;
  if (opt.max_ratio) fh.flags |= kFlagMaxRatio;
  if (opt.precomp) fh.flags |= kFlagPrecomp;
  fh.original_size = input.size();
  // A block is never empty, so the count is the ceiling of the division.
  fh.block_count =
      input.empty() ? 0 : (input.size() + opt.block_size - 1) / opt.block_size;

  ByteWriter w;
  w.reserve(input.size() + 64);
  write_file_header(w, fh);

  CompressStats local;
  size_t pos = 0;
  while (pos < input.size()) {
    const size_t n = std::min(opt.block_size, input.size() - pos);
    const std::span<const uint8_t> block = input.subspan(pos, n);

    std::vector<EncodedBlock> cands = encode_candidates(block, opt);
    auto best = std::min_element(
        cands.begin(), cands.end(), [](const EncodedBlock& a, const EncodedBlock& b) {
          return a.hdr.compressed_size < b.hdr.compressed_size;
        });

    write_block_header(w, best->hdr);
    w.bytes(best->payload);

    ++local.blocks;
    if (best->hdr.mode == Mode::Raw) ++local.raw_blocks;
    local.payload_bytes += best->hdr.compressed_size;

    pos += n;
  }

  out = w.take();
  if (stats) *stats = local;
  return Err::Ok;
}

}  // namespace chce
