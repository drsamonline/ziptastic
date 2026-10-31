// compressor.cpp — orchestrates the per-block encode loop and the Tier-1
// whole-stream LZMA2 fallback.
#include "compressor.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "codecs/lzma2_fallback.hpp"
#include "chimera/coarse_index.hpp"
#include "parser.hpp"
#include "token_stream.hpp"
#include "chimera/huffman.hpp"
#include "entropy/bwt_bsc.hpp"
#include "entropy/cm_zpaq.hpp"
#include "entropy/ppmd.hpp"
#include "entropy/token_coder.hpp"
#include "precompressors/bcj_x86.hpp"
#include "precompressors/dedup.hpp"
#include "precompressors/preflate_wrap.hpp"

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

uint8_t header_flags(const CompressOptions& opt, bool bcj, bool precomp,
                     bool parallel, bool dedup) {
  uint8_t f = 0;
  if (opt.coarse) f |= kFlagCoarse;
  if (opt.max_ratio) f |= kFlagMaxRatio;
  if (opt.precomp && precomp) f |= kFlagPrecomp;
  if (parallel) f |= kFlagParallel;
  if (dedup) f |= kFlagDedup;
  if (bcj) f |= kFlagBcj;
  return f;
}

// The CHCE-native encoding: one block per `block_size` chunk, each block
// optimally parsed against the fine match finder.
// Encodes one block into the smallest of its candidate encodings.
//
// Extracted so that archive mode and parallel mode share exactly one
// implementation. They differ only in the state handed in: archive mode passes
// the stream-wide finder, cost model and coarse index so matches can reach back
// across block boundaries; parallel mode passes a finder built over the block
// alone, which is what makes the blocks independent.
EncodedBlock encode_one_block(std::span<const uint8_t> input, size_t pos, size_t n,
                              MatchFinder& finder, const CostModel& model,
                              CoarseIndex* coarse, const CompressOptions& opt,
                              CompressStats* stats) {
  const std::span<const uint8_t> block = input.subspan(pos, n);
  std::vector<Token> tokens;

  std::vector<EncodedBlock> cands = encode_candidates(block, opt);
  if (opt.parse && n > 0) {
    const uint64_t cost = parse_block(input, pos, pos + n, finder, model,
                                      tokens, ParseOptions{},
                                      coarse);
    (void)cost;
    // Static and adaptive rANS both live in RANS_CTX; the body's first byte
    // says which model it is, so the smaller candidate can be taken freely.
    ByteWriter body;
    encode_tokens_rans(body, input, pos, pos + n, tokens);
    EncodedBlock pb;
    pb.hdr.mode = Mode::RansCtx;
    pb.hdr.uncompressed_size = n;
    pb.hdr.compressed_size = body.size();
    pb.body = body.take();
    stats->tokens += tokens.size();
    cands.push_back(std::move(pb));

    // PPMd: models context rather than position, which is why it is the text
    // backend. Offered per block so binary blocks never pay for it.
    {
      std::vector<uint8_t> pb;
      if (ppmd_compress_block(block, pb)) {
        EncodedBlock pbk;
        pbk.hdr.mode = Mode::Ppmd;
        pbk.hdr.uncompressed_size = n;
        pbk.hdr.compressed_size = pb.size();
        pbk.body = std::move(pb);
        cands.push_back(std::move(pbk));
      }
    }

    // Context mixing, only under --max-ratio: tens of times slower than
    // anything else here, and only worth paying when the caller asks for the
    // ratio ceiling.
    if (opt.max_ratio) {
      std::vector<uint8_t> zb;
      const std::vector<uint8_t> blk(block.begin(), block.end());
      if (zpaq_compress_block(blk, zb)) {
        EncodedBlock z;
        z.hdr.mode = Mode::CmZpaq;
        z.hdr.uncompressed_size = n;
        z.hdr.compressed_size = zb.size();
        z.body = std::move(zb);
        cands.push_back(std::move(z));
      }
    }

    // Burrows-Wheeler, via libbsc. A different family from LZ: it wins on
    // statistically structured binary and loses on ordinary text, so it is
    // offered per block and only kept where it is smallest.
    if (opt.bwt) {
      std::vector<uint8_t> bb;
      if (bsc_compress_block(block, bb)) {
        EncodedBlock b;
        b.hdr.mode = Mode::BwtBsc;
        b.hdr.uncompressed_size = n;
        b.hdr.compressed_size = bb.size();
        b.body = std::move(bb);
        cands.push_back(std::move(b));
      }
    }

    // Byte-level Huffman: no LZ, just the block's byte distribution. Only
    // worth offering where LZ finds little but the bytes are skewed.
    if (opt.huffman) {
      ByteWriter hs;
      huff_static_encode(block, hs);
      EncodedBlock hb;
      hb.hdr.mode = Mode::HuffStatic;
      hb.hdr.uncompressed_size = n;
      hb.hdr.compressed_size = hs.size();
      hb.body = hs.take();
      cands.push_back(std::move(hb));

      ByteWriter ha;
      huff_adaptive_encode(block, ha);
      EncodedBlock hab;
      hab.hdr.mode = Mode::HuffAdapt;
      hab.hdr.uncompressed_size = n;
      hab.hdr.compressed_size = ha.size();
      hab.body = ha.take();
      cands.push_back(std::move(hab));
    }

    if (opt.range) {
      ByteWriter rbody;
      encode_tokens_range(rbody, input, pos, pos + n, tokens);
      EncodedBlock rb;
      rb.hdr.mode = Mode::RansCtx;
      rb.hdr.uncompressed_size = n;
      rb.hdr.compressed_size = rbody.size();
      rb.body = rbody.take();
      cands.push_back(std::move(rb));
    }

    if (opt.adaptive) {
      ByteWriter abody;
      encode_tokens_rans_adaptive(abody, input, pos, pos + n, tokens);
      EncodedBlock ab;
      ab.hdr.mode = Mode::RansCtx;
      ab.hdr.uncompressed_size = n;
      ab.hdr.compressed_size = abody.size();
      ab.body = abody.take();
      cands.push_back(std::move(ab));
    }
  }
  auto best = std::min_element(
      cands.begin(), cands.end(), [](const EncodedBlock& a, const EncodedBlock& b) {
        return a.hdr.compressed_size < b.hdr.compressed_size;
      });
  return std::move(*best);
}

Err encode_native(std::span<const uint8_t> input, const CompressOptions& opt,
                  bool bcj, bool precomp, bool dedup, uint32_t checksum,
                  std::vector<uint8_t>& out, CompressStats& stats) {
  const size_t block_count =
      input.empty() ? 0 : (input.size() + opt.block_size - 1) / opt.block_size;
  const bool parallel = opt.parallel && block_count > 1;

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt, bcj, precomp, parallel, dedup);
  fh.original_size = input.size();
  fh.block_count = block_count;
  fh.checksum = checksum;

  ByteWriter w;
  w.reserve(input.size() + 64);
  write_file_header(w, fh);

  auto account = [&stats](const EncodedBlock& e) {
    if (e.hdr.mode == Mode::Raw) ++stats.raw_blocks;
    if (e.hdr.mode == Mode::Lzma2Fallback) ++stats.lzma2_blocks;
    if (e.hdr.mode == Mode::RansCtx) ++stats.parsed_blocks;
    if (e.hdr.mode == Mode::HuffStatic || e.hdr.mode == Mode::HuffAdapt) {
      ++stats.huff_blocks;
    }
    if (e.hdr.mode == Mode::BwtBsc) ++stats.bwt_blocks;
    ++stats.mode_counts[static_cast<uint8_t>(e.hdr.mode) & 15];
    ++stats.blocks;
  };

  const size_t nblocks = fh.block_count;

  if (parallel) {
    // Parallel mode. Every block is encoded against itself: its own match
    // finder over its own slice, no coarse index, no carried dictionary. Blocks
    // therefore reference only themselves, which is what makes them
    // independent — and it is the whole difference from archive mode.
    std::vector<EncodedBlock> encoded(nblocks);
    unsigned nt = opt.threads ? opt.threads : std::thread::hardware_concurrency();
    if (nt == 0) nt = 1;
    if (nt > nblocks) nt = static_cast<unsigned>(nblocks);

    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (;;) {
        const size_t b = next.fetch_add(1, std::memory_order_relaxed);
        if (b >= nblocks) break;
        const size_t pos = b * opt.block_size;
        const size_t n = std::min(opt.block_size, input.size() - pos);
        const std::span<const uint8_t> view = input.subspan(pos, n);
        MatchFinder::Options o;
        o.window = opt.window;
        o.nice_len = kMaxMatchLen;
        MatchFinder finder(view, o);
        StaticCostModel model(view, /*coarse_discount=*/0);
        CompressStats local;
        encoded[b] = encode_one_block(view, 0, n, finder, model, nullptr, opt, &local);
        stats.tokens += local.tokens;
      }
    };

    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned i = 0; i < nt; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    for (const auto& e : encoded) {
      write_block_header(w, e.hdr);
      w.bytes(e.body);
      account(e);
    }
  } else {
    // Archive mode: the dictionary carries across blocks and the coarse index
    // sees the whole stream, so matches can reach arbitrarily far back.
    MatchFinder::Options mf_opt;
    mf_opt.window = opt.window;
    mf_opt.nice_len = kMaxMatchLen;
    MatchFinder finder(input, mf_opt);
    StaticCostModel model(input, /*coarse_discount=*/0);
    CoarseIndex coarse(input);

    size_t pos = 0;
    while (pos < input.size()) {
      const size_t n = std::min(opt.block_size, input.size() - pos);
      EncodedBlock best = encode_one_block(input, pos, n, finder, model,
                                           opt.coarse ? &coarse : nullptr, opt,
                                           &stats);
      write_block_header(w, best.hdr);
      w.bytes(best.body);
      account(best);
      pos += n;
    }
  }

  out = w.take();
  return Err::Ok;
}

// The Tier-1 fallback: the whole input as a single raw LZMA2 stream inside a
// single LZMA2_FALLBACK block. Block body layout (docs/FORMAT.md):
//   props_len varint | props bytes | raw LZMA2 payload
Err encode_lzma2_stream(std::span<const uint8_t> input,
                        const CompressOptions& opt, bool bcj, bool precomp,
                        bool dedup, uint32_t checksum, std::vector<uint8_t>& out) {
  std::vector<uint8_t> payload;
  std::vector<uint8_t> props;
  if (Err e = lzma2_encode(input, payload, props); e != Err::Ok) return e;

  ByteWriter body;
  body.varint(props.size());
  body.bytes(props);
  body.bytes(payload);

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt, bcj, precomp, false, dedup);
  fh.original_size = input.size();
  fh.block_count = 1;
  fh.checksum = checksum;

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

// Decides whether the BCJ transform pays for this input by trying it on a
// sample rather than guessing from opcode density. A density heuristic is not
// enough: `mozilla` is a tar of executables and has plenty of CALL/JMP bytes,
// but the transform makes it 0.8% WORSE there, while on `ooffice` it is worth
// 13.9%. Two small compressions settle it in well under a second.
// Decides whether inflating a DEFLATE container actually pays. The payload is
// BIGGER than the input (it holds inflated data), so comparing raw sizes is
// meaningless — what matters is which one compresses smaller. Judged on a
// sample, which is cheap and decisive: inflated text compresses several times
// better than the DEFLATE stream it came from.
bool precomp_helps(std::span<const uint8_t> payload, std::span<const uint8_t> original) {
  const size_t kSample = 256 * 1024;
  auto coded = [](std::span<const uint8_t> sample) -> size_t {
    StaticCostModel model(sample);
    MatchFinder::Options o;
    o.window = 16u << 20;
    o.nice_len = kMaxMatchLen;
    MatchFinder finder(sample, o);
    std::vector<Token> tokens;
    parse_block(sample, 0, static_cast<uint32_t>(sample.size()), finder, model, tokens);
    ByteWriter w;
    encode_tokens_range(w, sample, 0, static_cast<uint32_t>(sample.size()), tokens);
    return w.size();
  };

  // Sample from the MIDDLE of each buffer, not the start: the payload begins
  // with the stream table and the preflate diffs, which are not representative
  // of the inflated data that dominates it. Sampling the start made a 38 MiB
  // payload of text look 60% less compressible than it is, and the decision
  // went the wrong way.
  auto mid = [kSample](std::span<const uint8_t> buf) {
    if (buf.size() <= kSample) return buf;
    const size_t off = (buf.size() - kSample) / 2;
    return buf.subspan(off, kSample);
  };
  const std::span<const uint8_t> psample = mid(payload);
  const std::span<const uint8_t> osample = mid(original);
  const size_t pn = psample.size();
  const size_t on = osample.size();
  if (pn < 4096 || on < 4096) return false;

  // Scale each sample's coded size to the full stream before comparing, since
  // the two samples cover different amounts of data.
  const double pay_est =
      static_cast<double>(coded(psample)) / pn * payload.size();
  const double orig_est =
      static_cast<double>(coded(osample)) / on * original.size();
  return pay_est < orig_est;
}

bool bcj_helps(std::span<const uint8_t> input) {
  const size_t kSample = 128 * 1024;
  const size_t n = std::min(input.size(), kSample);
  if (n < 4096) return false;

  std::span<const uint8_t> plain = input.first(n);
  std::vector<uint8_t> transformed(plain.begin(), plain.end());
  bcj_x86_transform(transformed, true);

  auto coded_size = [](std::span<const uint8_t> sample) -> size_t {
    StaticCostModel model(sample);
    MatchFinder::Options mf_opt;
    mf_opt.window = 16u << 20;
    mf_opt.nice_len = kMaxMatchLen;
    MatchFinder finder(sample, mf_opt);
    std::vector<Token> tokens;
    parse_block(sample, 0, static_cast<uint32_t>(sample.size()), finder, model,
                tokens);
    ByteWriter w;
    encode_tokens_range(w, sample, 0, static_cast<uint32_t>(sample.size()), tokens);
    return w.size();
  };

  const size_t plain_size = coded_size(plain);
  const size_t bcj_size = coded_size(transformed);
  return bcj_size < plain_size;
}

}  // namespace

Err compress(std::span<const uint8_t> input, std::vector<uint8_t>& out,
             const CompressOptions& opt, CompressStats* stats) {
  if (opt.block_size == 0 || opt.block_size > kMaxBlockSize) {
    return Err::BadBlockSize;
  }
  if (input.size() > kMaxStreamSize) return Err::BadBlockSize;

  // Checksum of the bytes as the caller handed them in. Every transform below
  // changes what the blocks hold, so this is the only value that can be checked
  // against the final decoded output.
  const uint32_t crc = crc32(input);

  // Layer 0a: DEFLATE recompression. A ZIP/PNG/JAR is already DEFLATE, so we
  // inflate it and compress the inflated bytes instead, keeping preflate's
  // correction so the exact original can be recreated. Only used when the round
  // trip is verified here, on this input — an unverified transform would be a
  // data-loss bug waiting to happen.
  std::vector<uint8_t> precomp_payload;
  bool precomp = false;
  if (opt.precomp && preflate_available() && !input.empty()) {
    const std::vector<uint8_t> in(input.begin(), input.end());
    std::vector<uint8_t> payload;
    if (preflate_unpack(in, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), input)) {
      precomp_payload = std::move(payload);
      precomp = true;
    }
  }

  // Layer 0: the x86 BCJ transform. Applied to the whole stream before
  // anything else, so the filter's state carries across block boundaries the
  // same way it does inside `xz --x86`. It is exactly invertible, so a wrong
  // guess costs ratio and never correctness.
  std::vector<uint8_t> transformed;
  // Everything downstream codes `data`: the inflated payload when precompression
  // applied, otherwise the original. The flag and the data must agree or the
  // decoder will run the inverse transform on bytes that never had it.
  std::span<const uint8_t> data =
      precomp ? std::span<const uint8_t>(precomp_payload) : input;
  const bool bcj = opt.bcj && bcj_x86_likely(data) && bcj_helps(data);
  if (bcj) {
    transformed.assign(data.begin(), data.end());
    bcj_x86_transform(transformed, true);
    data = transformed;
  }

  // Layer 0b: coarse dedup. Applied after BCJ so the filter still sees real x86
  // bytes, and before block splitting so every backend benefits from the
  // removed duplicates. Verified here before use, like every other transform.
  std::vector<uint8_t> dedup_payload;
  bool dedup = false;
  if (opt.dedup && data.size() >= (1u << 20)) {
    std::vector<uint8_t> packed;
    if (dedup_pack(data, packed) && packed.size() < data.size()) {
      std::vector<uint8_t> check;
      if (dedup_unpack(packed, check) && check.size() == data.size() &&
          std::memcmp(check.data(), data.data(), data.size()) == 0) {
        dedup_payload = std::move(packed);
        dedup = true;
      }
    }
  }
  if (dedup) data = dedup_payload;

  CompressStats local;
  std::vector<uint8_t> native;
  if (Err e = encode_native(data, opt, bcj, precomp, dedup, crc, native, local); e != Err::Ok) {
    return e;
  }

  // Tier-1 guarantee: if LZMA2 does better on the whole stream, ship that.
  // Comparison is on total stream size, framing included, so the promise is
  // about the bytes the user actually gets.
  if (opt.lzma2_fallback && lzma2_available() && !input.empty()) {
    std::vector<uint8_t> fallback;
    const Err e = encode_lzma2_stream(data, opt, bcj, precomp, dedup, crc, fallback);
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
