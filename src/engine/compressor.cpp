// compressor.cpp — orchestrates the per-block encode loop and the Tier-1
// whole-stream LZMA2 fallback.
#include "compressor.hpp"

#include <algorithm>
#include <cstring>

#include "codecs/lzma2_fallback.hpp"
#include "parser.hpp"
#include "token_stream.hpp"
#include "entropy/token_coder.hpp"

namespace chce {
namespace {

// One fully encoded block: header plus body, ready to serialise.
struct EncodedBlock {
  BlockHeader hdr;
  std::vector<uint8_t> body;  // metadata + payload
};

// RAW candidate: the block verbatim, no metadata, no entropy coding.
EncodedBlock encode_raw(std::span<const uint8_t> block) {
  EncodedBlock b;
  b.hdr.mode = Mode::Raw;
  b.hdr.uncompressed_size = block.size();
  b.hdr.compressed_size = block.size();
  b.body.assign(block.begin(), block.end());
  return b;
}

// The per-block candidate set. Each additional backend appends a candidate
// here and the smallest wins, so the selection rule stays in one place.
std::vector<EncodedBlock> encode_candidates(std::span<const uint8_t> block,
                                            const CompressOptions& opt) {
  (void)opt;
  std::vector<EncodedBlock> cands;
  cands.push_back(encode_raw(block));
  return cands;
}

uint8_t header_flags(const CompressOptions& opt) {
  uint8_t f = 0;
  if (opt.coarse) f |= kFlagCoarse;
  if (opt.max_ratio) f |= kFlagMaxRatio;
  if (opt.precomp) f |= kFlagPrecomp;
  return f;
}

// The CHCE-native encoding: one block per `block_size` chunk, each block
// optimally parsed against the fine match finder.
Err encode_native(std::span<const uint8_t> input, const CompressOptions& opt,
                  std::vector<uint8_t>& out, CompressStats& stats) {
  // The finder and the cost model live for the whole stream: the dictionary
  // carries across blocks, and the model is fitted to the whole input rather
  // than re-learned per block.
  MatchFinder::Options mf_opt;
  mf_opt.window = opt.window;
  mf_opt.nice_len = kMaxMatchLen;
  MatchFinder finder(input, mf_opt);
  StaticCostModel model(input);

  std::vector<Token> tokens;

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt);
  fh.original_size = input.size();
  fh.block_count =
      input.empty() ? 0 : (input.size() + opt.block_size - 1) / opt.block_size;

  ByteWriter w;
  w.reserve(input.size() + 64);
  write_file_header(w, fh);

  size_t pos = 0;
  while (pos < input.size()) {
    const size_t n = std::min(opt.block_size, input.size() - pos);
    const std::span<const uint8_t> block = input.subspan(pos, n);

    std::vector<EncodedBlock> cands = encode_candidates(block, opt);
    if (opt.parse && n > 0) {
      const uint64_t cost =
          parse_block(input, pos, pos + n, finder, model, tokens);
      (void)cost;
      ByteWriter body;
      encode_tokens_rans(body, input, pos, pos + n, tokens);
      EncodedBlock pb;
      pb.hdr.mode = Mode::RansCtx;
      pb.hdr.uncompressed_size = n;
      pb.hdr.compressed_size = body.size();
      pb.body = body.take();
      stats.tokens += tokens.size();
      cands.push_back(std::move(pb));
    }
    auto best = std::min_element(
        cands.begin(), cands.end(), [](const EncodedBlock& a, const EncodedBlock& b) {
          return a.hdr.compressed_size < b.hdr.compressed_size;
        });

    write_block_header(w, best->hdr);
    w.bytes(best->body);

    ++stats.blocks;
    if (best->hdr.mode == Mode::Raw) ++stats.raw_blocks;
    if (best->hdr.mode == Mode::Lzma2Fallback) ++stats.lzma2_blocks;
    if (best->hdr.mode == Mode::RansCtx) ++stats.parsed_blocks;
    stats.payload_bytes += best->hdr.compressed_size;

    pos += n;
  }

  out = w.take();
  return Err::Ok;
}

// The Tier-1 fallback: the whole input as a single raw LZMA2 stream inside a
// single LZMA2_FALLBACK block. Block body layout (docs/FORMAT.md):
//   props_len varint | props bytes | raw LZMA2 payload
Err encode_lzma2_stream(std::span<const uint8_t> input,
                        const CompressOptions& opt, std::vector<uint8_t>& out) {
  std::vector<uint8_t> payload;
  std::vector<uint8_t> props;
  if (Err e = lzma2_encode(input, payload, props); e != Err::Ok) return e;

  ByteWriter body;
  body.varint(props.size());
  body.bytes(props);
  body.bytes(payload);

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt);
  fh.original_size = input.size();
  fh.block_count = 1;

  BlockHeader bh;
  bh.mode = Mode::Lzma2Fallback;
  bh.uncompressed_size = input.size();
  bh.compressed_size = body.size();

  ByteWriter w;
  w.reserve(body.size() + 32);
  write_file_header(w, fh);
  write_block_header(w, bh);
  w.bytes(body.span());

  out = w.take();
  return Err::Ok;
}

}  // namespace

Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats) {
  if (opt.block_size == 0 || opt.block_size > kMaxBlockSize) {
    return Err::BadBlockSize;
  }
  if (input.size() > kMaxStreamSize) return Err::BadBlockSize;

  CompressStats local;
  std::vector<uint8_t> native;
  if (Err e = encode_native(input, opt, native, local); e != Err::Ok) return e;

  // Tier-1 guarantee: if LZMA2 does better on the whole stream, ship that.
  // Comparison is on total stream size, framing included, so the promise is
  // about the bytes the user actually gets.
  if (opt.lzma2_fallback && lzma2_available() && !input.empty()) {
    std::vector<uint8_t> fallback;
    const Err e = encode_lzma2_stream(input, opt, fallback);
    if (e == Err::Ok && fallback.size() < native.size()) {
      CompressStats fb;
      fb.blocks = 1;
      fb.lzma2_blocks = 1;
      fb.payload_bytes = fallback.size();
      fb.lzma2_stream = true;
      out = std::move(fallback);
      if (stats) *stats = fb;
      return Err::Ok;
    }
    // A codec failure is not fatal: the native encoding is still valid, so we
    // fall through rather than failing the whole compression.
  }

  out = std::move(native);
  if (stats) *stats = local;
  return Err::Ok;
}

}  // namespace chce
