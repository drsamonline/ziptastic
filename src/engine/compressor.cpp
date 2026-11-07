// compressor.cpp — orchestrates the per-block encode loop and the Tier-1
// whole-stream LZMA2 fallback.
#include "compressor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include "codecs/lzma2_fallback.hpp"
#include "chimera/coarse_index.hpp"
#include "chimera/sha256.hpp"
#include "parser.hpp"
#include "token_stream.hpp"
#include "chimera/huffman.hpp"
#include "entropy/bwt_bsc.hpp"
#include "entropy/cm_zpaq.hpp"
#include "entropy/ppmd.hpp"
#include "entropy/token_coder.hpp"
#include "precompressors/bcj_x86.hpp"
#include "precompressors/dedup.hpp"
#include "precompressors/gif_wrap.hpp"
#include "precompressors/numeric_wrap.hpp"
#include "precompressors/xml_numeric_wrap.hpp"
#include "precompressors/base64_wrap.hpp"
#include "precompressors/wav_wrap.hpp"
#include "precompressors/sqlite_wrap.hpp"
#include "precompressors/png_wrap.hpp"
#include "precompressors/ecc_wrap.hpp"
#include "precompressors/bmp_wrap.hpp"
#include "precompressors/gzip_wrap.hpp"
#include "precompressors/xz_wrap.hpp"
#include "precompressors/brotli_wrap.hpp"
#include "precompressors/jpeg_wrap.hpp"
#include "plugin_host.hpp"
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
                     bool parallel, bool dedup, bool jpeg, bool gif) {
  uint8_t f = 0;
  if (opt.coarse) f |= kFlagCoarse;
  if (opt.max_ratio) f |= kFlagMaxRatio;
  if (opt.precomp && precomp) f |= kFlagPrecomp;
  if (parallel) f |= kFlagParallel;
  if (dedup) f |= kFlagDedup;
  if (jpeg) f |= kFlagJpeg;
  if (gif) f |= kFlagGif;
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
// Would the byte-level Huffman coder have a chance on this block?
//
// Huffman costs 18% of a text run and wins nothing there, while on a seven-symbol file it
// wins 0.91% — so it can be neither left on nor turned off (SELECTOR_PROFILE.md). The
// trial is a measurement, not a rule: encode a 64 KiB prefix with the coder that would
// actually run, scale the result to the whole block, and run the full candidate only if
// that estimate is within kSlackPct of the best candidate so far.
//
// A prefix, not a middle slice, because a byte histogram is a whole-block property and
// any slice represents it; the slack is what pays for the difference. It is deliberately
// generous in Huffman's favour: the cost of running it needlessly is time, the cost of
// skipping it wrongly is ratio, and this project has been wrong in the ratio direction
// three times.
bool huffman_may_win(std::span<const uint8_t> block, size_t best_so_far) {
  constexpr size_t kSample = 64 * 1024;
  constexpr size_t kSlackPct = 1;
  // Small blocks are not worth a trial: the estimate from a fraction of a small block is
  // worse than the thing it estimates.
  if (block.size() <= kSample) return true;
  const std::span<const uint8_t> sample = block.subspan(0, kSample);
  ByteWriter w;
  huff_static_encode(sample, w);
  const uint64_t est = static_cast<uint64_t>(w.size()) * block.size() / sample.size();
  return est * 100 <= static_cast<uint64_t>(best_so_far) * (100 + kSlackPct);
}

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

    // Backends from plugins (include/chimera/plugin.hpp). These are the
    // copyleft ones: LGPL permits use from an MIT program when the library is
    // separately replaceable, which is exactly what a .so is. Loaded once.
    {
      static const std::vector<LoadedPlugin> plugins = load_plugins(default_plugin_dir());
      for (const LoadedPlugin& p : plugins) {
        const size_t cap = p.api->bound(n);
        if (cap == 0) continue;
        std::vector<uint8_t> buf(cap);
        const long got = p.api->compress(block.data(), n, buf.data(), cap);
        if (got <= 0 || static_cast<size_t>(got) >= n) continue;
        buf.resize(static_cast<size_t>(got));

        ByteWriter bw;
        bw.varint(p.name.size());
        bw.bytes(reinterpret_cast<const uint8_t*>(p.name.data()), p.name.size());
        bw.bytes(buf.data(), buf.size());

        EncodedBlock pb2;
        pb2.hdr.mode = Mode::Plugin;
        pb2.hdr.uncompressed_size = n;
        pb2.hdr.compressed_size = bw.size();
        pb2.body = bw.take();
        cands.push_back(std::move(pb2));
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
    // worth offering where LZ finds little but the bytes are skewed — and only after a
    // trial says so, because it is the one candidate that costs real time to lose.
    size_t best_so_far = n;
    for (const EncodedBlock& c : cands) {
      best_so_far = std::min(best_so_far, static_cast<size_t>(c.hdr.compressed_size));
    }
    if (opt.huffman && opt.huff_static) ++stats->huff_trials;
    if (opt.huffman && opt.huff_static && huffman_may_win(block, best_so_far)) {
      ByteWriter hs;
      huff_static_encode(block, hs);
      EncodedBlock hb;
      hb.hdr.mode = Mode::HuffStatic;
      hb.hdr.uncompressed_size = n;
      hb.hdr.compressed_size = hs.size();
      hb.body = hs.take();
      cands.push_back(std::move(hb));
    } else if (opt.huffman && opt.huff_static) {
      ++stats->huff_skipped;
    }
    if (opt.huffman && opt.huff_adaptive) {
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
                  bool bcj, bool precomp, bool dedup, bool jpeg, bool gif,
                  uint8_t layers, uint8_t layers2, uint8_t layers3,
                  uint32_t checksum, uint64_t true_original_size,
                  std::vector<uint8_t>& out, CompressStats& stats) {
  const size_t block_count =
      input.empty() ? 0 : (input.size() + opt.block_size - 1) / opt.block_size;
  const bool parallel = opt.parallel && block_count > 1;

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt, bcj, precomp, parallel, dedup, jpeg, gif);
  fh.layers = layers;
  fh.layers2 = layers2;
  fh.layers3 = layers3;
  fh.original_size = input.size();   // post-Layer-0: what the block loop encodes
  fh.true_size = true_original_size; // what the caller passed in
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

  // Every block is sealed the same way: digest its plaintext, then record the
  // header in the manifest. One helper so the four write sites cannot disagree
  // about what a block's hash covers.
  BlockManifest manifest;
  auto seal = [&manifest](BlockHeader& h, std::span<const uint8_t> plaintext) {
    h.sha256 = sha256(plaintext);
    manifest.add(h);
  };

  // Write a block, and refuse to write one whose header disagrees with its body.
  //
  // Every candidate sets `compressed_size` by hand, and nothing checked it against the
  // bytes actually written: the digest covers the plaintext, not this. A backend added
  // later that sets the size before a final tweak to the body produces a stream whose
  // header lies, and the decoder reads the wrong number of bytes — the next block's
  // header lands mid-payload and the failure surfaces somewhere unrelated. One comparison
  // here turns that into an immediate, named error at the only place it can be caught.
  //
  // Where each block's header starts, stream-relative, for the v8 trailer's offset table.
  // Recorded here rather than at the two call sites because this is the only place that
  // knows the position, and a table written from a second place would eventually disagree
  // with the bytes it describes.
  std::vector<uint64_t> offsets;
  offsets.reserve(nblocks);
  auto write_block = [&offsets](ByteWriter& out, const EncodedBlock& e) -> Err {
    if (e.hdr.compressed_size != e.body.size()) return Err::SizeMismatch;
    offsets.push_back(out.size());
    write_block_header(out, e.hdr);
    out.bytes(e.body);
    return Err::Ok;
  };

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
    // Per-block token counts, summed after the join. The obvious `stats.tokens +=
    // local.tokens` inside the worker is a data race on a shared struct — undefined
    // behaviour, and a lost update in practice, so the `-v` token count drifted with the
    // thread scheduling. Everything else the workers touch is either indexed by block
    // (`encoded[b]`) or constructed inside the worker.
    std::vector<uint64_t> tokens_by_block(nblocks, 0);
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
        tokens_by_block[b] = local.tokens;
      }
    };

    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned i = 0; i < nt; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    for (uint64_t t : tokens_by_block) stats.tokens += t;

    for (size_t b = 0; b < nblocks; ++b) {
      const size_t pos = b * opt.block_size;
      const size_t n = std::min(opt.block_size, input.size() - pos);
      seal(encoded[b].hdr, input.subspan(pos, n));
      if (Err e = write_block(w, encoded[b]); e != Err::Ok) return e;
      account(encoded[b]);
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
      seal(best.hdr, input.subspan(pos, n));
      if (Err e = write_block(w, best); e != Err::Ok) return e;
      account(best);
      pos += n;
    }
  }

  write_trailer(w, fh.block_count, manifest.digest(), offsets);
  out = w.take();
  return Err::Ok;
}

// The Tier-1 fallback: the whole input as a single raw LZMA2 stream inside a
// single LZMA2_FALLBACK block. Block body layout (docs/FORMAT.md):
//   props_len varint | props bytes | raw LZMA2 payload
Err encode_lzma2_stream(std::span<const uint8_t> input,
                        const CompressOptions& opt, bool bcj, bool precomp,
                        bool dedup, bool jpeg, bool gif, uint8_t layers,
                        uint8_t layers2, uint8_t layers3, uint32_t checksum,
                        uint64_t true_original_size, std::vector<uint8_t>& out) {
  std::vector<uint8_t> payload;
  std::vector<uint8_t> props;
  if (Err e = lzma2_encode(input, payload, props); e != Err::Ok) return e;

  ByteWriter body;
  body.varint(props.size());
  body.bytes(props);
  body.bytes(payload);

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt, bcj, precomp, false, dedup, jpeg, gif);
  fh.layers = layers;
  fh.layers2 = layers2;
  fh.layers3 = layers3;
  fh.original_size = input.size();
  fh.true_size = true_original_size;
  fh.block_count = 1;
  fh.checksum = checksum;

  BlockHeader bh;
  bh.mode = Mode::Lzma2Fallback;
  bh.uncompressed_size = input.size();
  bh.compressed_size = body.size();

  BlockManifest manifest;
  bh.sha256 = sha256(input);
  manifest.add(bh);

  ByteWriter w;
  w.reserve(body.size() + 128);
  write_file_header(w, fh);
  const uint64_t block_at = w.size();
  write_block_header(w, bh);
  w.bytes(body.span());
  write_trailer(w, 1, manifest.digest(), std::span<const uint64_t>(&block_at, 1));

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
    size_t best = w.size();
    // The estimate must be able to see what the engine will actually choose. On
    // XML and other structured text the engine picks BWT and beats the range-coder
    // token path by ~2x; measuring only the token path made this gate decline a
    // transform worth -50% on a nested XLSX. This is the third time a predictor has
    // stood in for a measurement and been wrong in the direction that costs ratio.
    if (bsc_available()) {
      std::vector<uint8_t> bb;
      if (bsc_compress_block(sample, bb) && bb.size() < best) best = bb.size();
    }
    return best;
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
  // Below this size, stop estimating and just code both in full. The sample
  // estimate is a predictor, and predictors have been the source of every wrong
  // decision in this project (the sample selector cost +2.68% size). A nested
  // XLSX is the case that exposed it: the estimate declined a transform worth
  // -50%, because the inflated XML's cost was extrapolated from a 256 KiB slice
  // that did not represent 3.2 MiB of repeating row markup.
  constexpr size_t kExactBelow = 8u << 20;
  // A structural signal beats a predictor here. If inflating expanded the data by
  // 2x or more, the original was DEFLATE over something highly compressible, and
  // taking the payload is right almost regardless of what the estimator says.
  //
  // This exists because the estimator is demonstrably biased on structured text:
  // on a nested XLSX it scored the payload 390,766 while the real engine scored the
  // same bytes at 181,633 - a 2.1x pessimism that made the gate decline a transform
  // worth 2x. That is the third predictor failure in this project and the second
  // that cost ratio; the honest conclusion is to stop predicting where a structural
  // fact is available.
  //
  // What this rule can still get wrong, and where that is caught: a payload that grew 2x
  // and codes worse than the original loses ratio, not data. Under 2 MiB — the Tier-0 guard
  // in `compress()` — the encoder builds the untransformed stream as well and keeps the
  // smaller, so the loss cannot reach the output; above it, nothing does, and this line
  // decides alone. That
  // is the honest statement of the risk — it is a predictor, and it is the only one left
  // in this file that can be wrong in the direction that costs size. It has not been
  // caught being wrong: every measured regression came from the estimator above, not from
  // this line. Making it measure would mean a full encode of both streams at any size.
  if (payload.size() >= original.size() * 2) return true;

  if (payload.size() <= kExactBelow && original.size() <= kExactBelow) {
    return coded(payload) < coded(original);
  }

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
    size_t best = w.size();
    // The estimate must be able to see what the engine will actually choose. On
    // XML and other structured text the engine picks BWT and beats the range-coder
    // token path by ~2x; measuring only the token path made this gate decline a
    // transform worth -50% on a nested XLSX. This is the third time a predictor has
    // stood in for a measurement and been wrong in the direction that costs ratio.
    if (bsc_available()) {
      std::vector<uint8_t> bb;
      if (bsc_compress_block(sample, bb) && bb.size() < best) best = bb.size();
    }
    return best;
  };

  const size_t plain_size = coded_size(plain);
  const size_t bcj_size = coded_size(transformed);
  return bcj_size < plain_size;
}

}  // namespace

// What this engine will actually pay for these bytes: run the real pipeline on
// them, with base64 itself switched off so the trial cannot recurse, and take the
// size it produces. Not an estimate of the pipeline — the pipeline.
//
// Two earlier versions of this gate were estimates, and both were wrong in the
// direction that costs ratio. The first used the range coder and BWT alone and
// missed 19.6% on a JPEG, because brunsli was not in the model. The second added
// JPEG, GIF and DEFLATE by hand and still got the XML case backwards, because the
// spreadsheet transform — which is most of what makes that payload cheap — was not
// in the model either. The lesson is the one this project keeps relearning: there
// is no cheap stand-in for running the thing.
size_t actual_cost(std::span<const uint8_t> s, const CompressOptions& opt) {
  CompressOptions local = opt;
  local.base64 = false;     // the trial must not recurse into itself
  local.max_ratio = false;  // and must not pay for the ratio ceiling
  // The Tier-1 LZMA2 fallback STAYS on. Turning it off was the last version's
  // mistake: on a 256 KiB prefix of the base64 text the native encoder costs
  // 16,412 and LZMA2 costs 10,128, so measuring only the native side made a
  // clear loss look like a tie. The comparison has to be between the streams the
  // engine would actually write.
  std::vector<uint8_t> out;
  CompressStats st;
  if (compress(s, out, local, &st) != Err::Ok) return static_cast<size_t>(-1);
  return out.size();
}

// Does decoding the base64 actually pay? Both sides are measured through the
// encoder on a bounded sample and scaled, the same way precomp_helps decides.
// The answer is genuinely payload-dependent: -19.6% on a dense payload, -48.4% on
// wrapped structured text, and a LOSS on the same structured text unwrapped. The
// first version of this gate estimated with the range coder and BWT alone and got
// both directions wrong — it missed 19.6% on a JPEG (because brunsli, which would
// actually fire, was not in the model) and accepted a 5.8% loss on JSON.
// A decisive first pass, or no opinion.
//
// A 64 KiB sample of this pipeline carries about 6% of error: an earlier version of this
// gate used 64 KiB and flipped both XML cases to "take", a 5.8% loss. So a 64 KiB answer is
// only taken when the two sides differ by three times that — 20% — which no sampling error
// of 6% can manufacture. Everything closer goes to the full trial below, and that includes
// the case that dominates the cost: two sides that are a tie, where the expensive pass runs
// in full only to confirm what the cheap one already suspects.
int base64_verdict_cheaply(std::span<const uint8_t> packed, std::span<const uint8_t> original,
                           const CompressOptions& opt) {
  constexpr size_t kCheapSample = 64 * 1024;
  constexpr double kDecisive = 1.20;  // 3x the measured error of a sample this size
  const size_t pn = std::min(packed.size(), kCheapSample);
  const size_t on = std::min(original.size(), kCheapSample);
  if (pn < 256 || on < 256) return -1;
  // One block per side: the block loop is most of what a trial costs, and a single block
  // still runs the transforms that decide the answer.
  CompressOptions one = opt;
  one.block_size = kCheapSample;
  const double p =
      static_cast<double>(actual_cost(packed.first(pn), one)) / pn * packed.size();
  const double o =
      static_cast<double>(actual_cost(original.first(on), one)) / on * original.size();
  if (p * kDecisive < o) return 1;   // taking it is a clear win
  if (o * kDecisive < p) return 0;   // taking it is a clear loss
  return -1;                         // too close to call at this size
}

bool base64_pays(std::span<const uint8_t> packed, std::span<const uint8_t> original,
                 const CompressOptions& opt) {
  // The cheap pass first, for the files where the answer is obvious — and only those.
  // It is worth having because the expensive trial below costs 2.1-2.2x the whole run: on a
  // 540 KB file of base64'd random bytes it spends 6.3 s to conclude a tie, and on a 34 KB
  // file of base64'd text it spends 0.9 s to find a 30% win. The cheap pass settles the
  // second kind and cannot touch the first, which is the honest shape of the saving.
  switch (base64_verdict_cheaply(packed, original, opt)) {
    case 1: return true;
    case 0: return false;
    default: break;
  }
  // 256 KiB, not less. At 64 KiB this trial flipped both XML cases back to
  // "take" — a 5.8% loss — because the margin it has to resolve is only 6% and a
  // small sample cannot resolve it. The cost of the bigger sample is measured and
  // recorded in docs/BASE64_INTEGRATION.md.
  const size_t kSample = 256 * 1024;
  const size_t pn = std::min(packed.size(), kSample);
  const size_t on = std::min(original.size(), kSample);
  // 256 bytes, not 1024. A small document with an embedded blob is exactly the case
  // this transform is for, and a 1 KiB floor silently excluded it — a 1,081-byte JPEG
  // in a 843-byte `.json.gz` declined while the same content at 21 KB gained 19.7%.
  if (pn < 256 || on < 256) return false;
  const double p =
      static_cast<double>(actual_cost(packed.first(pn), opt)) / pn * packed.size();
  const double o =
      static_cast<double>(actual_cost(original.first(on), opt)) / on * original.size();
  return p < o;
}

// Should the Tier-1 LZMA2 fallback run in full?
//
// It is a guarantee, not an optimisation — the promise that a stream is never much worse
// than an LZMA-class compressor — and it costs 43% of a run on a mixed asset tree where it
// wins nothing (measured below). It cannot be turned off, and it cannot be gated the way
// the Huffman candidate was: skipping when the sample is *close* would break the promise
// exactly where the sample is least representative.
//
// So it declines only when a sample says LZMA2 is far out of reach — 20% worse, scaled to
// the whole stream. Two things make that margin conservative rather than arbitrary: the
// sample is a real LZMA2 encode at the same settings, and a slice always encodes worse than
// the whole stream because its dictionary starts cold, so the estimate is biased against
// LZMA2 and the margin is spent correcting a bias that points the safe way.
//
// The sample is 64 KiB because liblzma's setup, not its coding, is what the trial pays for:
// at 256 KiB the trial cost 1.4 s on a 1.6 MB archive, within a third of the run it was
// trying to avoid.
bool lzma2_may_win(std::span<const uint8_t> input, size_t native_size) {
  constexpr size_t kSample = 64 * 1024;
  constexpr size_t kSlackPct = 20;
  // Below this the trial costs more than the run it would skip.
  if (input.size() <= kSample * 2) return true;

  const size_t off = (input.size() - kSample) / 2;
  const std::span<const uint8_t> sample = input.subspan(off, kSample);
  std::vector<uint8_t> payload;
  std::vector<uint8_t> props;
  if (lzma2_encode(sample, payload, props) != Err::Ok) return true;  // no opinion: run it
  const uint64_t est =
      static_cast<uint64_t>(payload.size()) * input.size() / sample.size();
  return est * 100 <= static_cast<uint64_t>(native_size) * (100 + kSlackPct);
}

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

  // Layer 0 — base64. Outermost of all, because every stage after it wants the
  // decoded bytes: a JPEG at offset 0 of the payload is what lets brunsli fire,
  // and blob-first measured 20.2% against 2.3% for skeleton-first.
  //
  // A trial, not a rule. Base64 of 200 KB of spreadsheet XML on one line
  // compresses BETTER than the XML itself, so the transform loses 6.1% there,
  // while the same payload wrapped at 76 columns is worth 48.4%. The difference
  // is the line breaks, not the 33% inflation.
  std::vector<uint8_t> base64_payload;
  bool base64 = false;
  if (opt.base64 && base64_available() && !input.empty()) {
    std::vector<uint8_t> packed;
    if (base64_pack(input, packed)) {
      std::vector<uint8_t> check;
      if (base64_unpack(packed, check) && check.size() == input.size() &&
          std::memcmp(check.data(), input.data(), input.size()) == 0 &&
          base64_pays(packed, input, opt)) {
        base64_payload = std::move(packed);
        base64 = true;
      }
    }
  }

  // Everything below sees the decoded payload when base64 fired, exactly as it
  // would if the caller had handed us the binary file.
  const std::span<const uint8_t> stage_in =
      base64 ? std::span<const uint8_t>(base64_payload) : input;

  // Layer 0 — integer CSV/TSV. It needs to see raw text, which means it runs after
  // base64 and before anything that would re-frame the bytes.
  //
  // ---------------------------------------------------------------------------
  // THE ORDER OF THE MIDDLE STAGES IS NOT A DERIVATION — READ THIS BEFORE MOVING ONE
  //
  // Only two positions in this chain are forced:
  //
  //   1. base64 runs FIRST, because every stage after it wants the decoded payload (a
  //      JPEG at offset 0 is what lets brunsli fire, worth 20.2% against 2.3%).
  //   2. the container stages run LAST, because precomp and gzip need the stream.
  //
  // Everything between them — numeric, wav, bmp, ecc, png, sqlite, jpeg, gif — is in the
  // order it is in because that is the order they were written, and it is safe ONLY
  // because those stages are mutually exclusive: a file cannot be a WAV and a PNG, and a
  // CSV cannot be a raw CD image. The decoder undoes them in a different order (numeric
  // before gif/jpeg/bmp, for instance) and that is correct for exactly the same reason.
  //
  // So: if you ever add a stage that can fire on a payload another stage has already
  // transformed, the decode order becomes load-bearing and this comment is the warning.
  // The audit (`docs/AUDIT_PLAN.md`, A2) records this as the chain's one unstated
  // dependency.
  std::vector<uint8_t> numeric_payload;
  // `numeric` means numeric_payload holds the stage-0 payload, whichever transform
  // produced it: the downstream stages pick the payload on this flag, so it has to
  // cover both variants or they code the wrong bytes while the layer bit claims a
  // transform ran. `numeric_csv` is only for the layer bit itself.
  bool numeric = false;
  bool numeric_csv = false;
  bool xml_numeric = false;
  if (opt.numeric && numeric_available() && !stage_in.empty()) {
    std::vector<uint8_t> packed;
    // Spreadsheet XML first: an .xlsx that precomp never inflated still shows up
    // here as a bare sheet1.xml, and the CSV transform cannot see inside <v> tags.
    if (xml_numeric_available() && xml_numeric_pack(stage_in, packed)) {
      std::vector<uint8_t> check;
      if (xml_numeric_unpack(packed, check) && check.size() == stage_in.size() &&
          std::memcmp(check.data(), stage_in.data(), stage_in.size()) == 0) {
        numeric_payload = std::move(packed);
        numeric = true;
        xml_numeric = true;
      }
    } else if (numeric_pack(stage_in, packed)) {
      std::vector<uint8_t> check;
      if (numeric_unpack(packed, check) && check.size() == stage_in.size() &&
          std::memcmp(check.data(), stage_in.data(), stage_in.size()) == 0) {
        numeric_payload = std::move(packed);
        numeric = true;
        numeric_csv = true;
      }
    }
  }

  // Layer 0 — 16-bit PCM WAV. Per-channel delta, which measured -16.5% on tonal
  // music and only -2.0% on noise, so it goes through the same accept/reject trial
  // as base64 rather than being applied to every WAV.
  std::vector<uint8_t> wav_payload;
  bool wav = false;
  if (opt.wav && wav_available() && !stage_in.empty() && wav_likely(stage_in)) {
    std::vector<uint8_t> packed;
    if (wav_pack(stage_in, packed)) {
      std::vector<uint8_t> check;
      if (wav_unpack(packed, check) && check.size() == stage_in.size() &&
          std::memcmp(check.data(), stage_in.data(), stage_in.size()) == 0 &&
          base64_pays(packed, stage_in, opt)) {
        wav_payload = std::move(packed);
        wav = true;
      }
    }
  }
  const std::span<const uint8_t> stage_wav =
      wav ? std::span<const uint8_t>(wav_payload) : stage_in;

  // Layer 0 — BMP pixels. Uncompressed BMP is interleaved BGR rows; splitting the
  // channels and delta-coding each row turns a photographic image into something the
  // backend can model. Gated by precomp_helps because a flat sprite sheet is already
  // trivial to compress and the split destroys the row structure it was using.
  std::vector<uint8_t> bmp_payload;
  bool bmp = false;
  if (opt.bmp && bmp_available() && !stage_wav.empty() && bmp_likely(stage_wav)) {
    std::vector<uint8_t> payload;
    if (bmp_pack(stage_wav, payload)) {
      std::vector<uint8_t> check;
      if (bmp_unpack(payload, check) && check.size() == stage_wav.size() &&
          std::memcmp(check.data(), stage_wav.data(), stage_wav.size()) == 0 &&
          precomp_helps(payload, stage_wav)) {
        bmp_payload = std::move(payload);
        bmp = true;
      }
    }
  }
  const std::span<const uint8_t> stage_bmp =
      bmp ? std::span<const uint8_t>(bmp_payload) : stage_wav;

  // Layer 0 — CD sector parity. A raw CD image carries 276 bytes of Reed-Solomon parity
  // per 2,352-byte sector that are fully determined by the 2,064 bytes before them.
  // Recomputing them is verified per sector before use, so a wrong table cannot pass.
  std::vector<uint8_t> ecc_payload;
  bool ecc = false;
  if (opt.ecc && ecc_available() && !stage_bmp.empty() && ecc_likely(stage_bmp)) {
    std::vector<uint8_t> packed;
    if (ecc_pack(stage_bmp, packed)) {
      std::vector<uint8_t> check;
      if (ecc_unpack(packed, check) && check.size() == stage_bmp.size() &&
          std::memcmp(check.data(), stage_bmp.data(), stage_bmp.size()) == 0) {
        ecc_payload = std::move(packed);
        ecc = true;
      }
    }
  }
  const std::span<const uint8_t> stage_ecc =
      ecc ? std::span<const uint8_t>(ecc_payload) : stage_bmp;

  // Layer 0 — PNG. A PNG is a DEFLATE stream inside a chunk list, so this is the
  // precomp pattern applied to a format precomp never reached. Measured at -26.2% on
  // a sprite sheet and -69.0% on a gradient image.
  std::vector<uint8_t> png_payload;
  bool png = false;
  if (opt.precomp && png_available() && !stage_ecc.empty() &&
      png_likely(std::vector<uint8_t>(stage_ecc.begin(), stage_ecc.end()))) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> png_in(stage_ecc.begin(), stage_ecc.end());
    if (png_unpack(png_in, payload)) {
      std::vector<uint8_t> check;
      if (png_repack(payload, check) && check.size() == stage_ecc.size() &&
          std::memcmp(check.data(), stage_ecc.data(), stage_ecc.size()) == 0 &&
          precomp_helps(payload, stage_ecc)) {
        png_payload = std::move(payload);
        png = true;
      }
    }
  }
  const std::span<const uint8_t> stage_png =
      png ? std::span<const uint8_t>(png_payload) : stage_ecc;

  // Layer 0 — SQLite pages grouped by type. A reordering, so it is cheap and its
  // recipe is 12 bytes; the gain is 5-6%, which is small for this project but the
  // file class had nothing before it.
  std::vector<uint8_t> sqlite_payload;
  bool sqlite = false;
  if (opt.sqlite && sqlite_available() && !stage_png.empty() && sqlite_likely(stage_png)) {
    std::vector<uint8_t> packed;
    if (sqlite_pack(stage_png, packed)) {
      std::vector<uint8_t> check;
      if (sqlite_unpack(packed, check) && check.size() == stage_png.size() &&
          std::memcmp(check.data(), stage_png.data(), stage_png.size()) == 0) {
        sqlite_payload = std::move(packed);
        sqlite = true;
      }
    }
  }
  const std::span<const uint8_t> stage_sql =
      sqlite ? std::span<const uint8_t>(sqlite_payload) : stage_png;

  // Layer 0 — JPEG. brunsli re-encodes the DCT coefficients and keeps a recipe that
  // reproduces the file exactly; verified here before it is trusted, like every other
  // transform.
  //
  // This comment used to begin "First, because a JPEG is not DEFLATE...", which stopped
  // being true when the image stages were added ahead of it. The order is not a
  // preference — see the note above `stage_in` for what actually makes it safe.
  std::vector<uint8_t> jpeg_payload;
  bool jpeg = false;
  if (opt.jpeg && jpeg_available() && !stage_sql.empty()) {
    const std::vector<uint8_t> in(stage_sql.begin(), stage_sql.end());
    std::vector<uint8_t> packed;
    if (jpeg_pack(in, packed)) {
      std::vector<uint8_t> check;
      if (jpeg_unpack(packed, check) && check.size() == in.size() &&
          std::memcmp(check.data(), in.data(), in.size()) == 0) {
        jpeg_payload = std::move(packed);
        jpeg = true;
      }
    }
  }

  // Layer 0 — GIF. Same reasoning as JPEG: LZW is a 1980s coder and the palette
  // indices behind it compress far better through this engine. Only applied when
  // the original LZW stream can be reproduced byte-exactly.
  std::vector<uint8_t> gif_payload;
  bool gif = false;
  {
    const std::span<const uint8_t> src =
        numeric ? std::span<const uint8_t>(numeric_payload)
                : (jpeg ? std::span<const uint8_t>(jpeg_payload) : stage_sql);
    if (opt.gif && gif_available() && !src.empty()) {
      std::vector<uint8_t> packed;
      if (gif_pack(src, packed)) {
        std::vector<uint8_t> check;
        if (gif_unpack(packed, check) && check.size() == src.size() &&
            std::memcmp(check.data(), src.data(), src.size()) == 0) {
          gif_payload = std::move(packed);
          gif = true;
        }
      }
    }
  }

  // Layer 0a: DEFLATE recompression. A ZIP/PNG/JAR is already DEFLATE, so we
  // inflate it and compress the inflated bytes instead, keeping preflate's
  // correction so the exact original can be recreated. Only used when the round
  // trip is verified here, on this input — an unverified transform would be a
  // data-loss bug waiting to happen.
  std::vector<uint8_t> precomp_payload;
  bool precomp = false;
  // True when the container gate accepted its payload on the STRUCTURAL rule (the inflated
  // payload is at least twice the size of what it came from) rather than on a measured
  // comparison. That rule is a predictor, and predictors here have a history: it is the one
  // path that can accept a payload the engine then encodes worse than the original. The
  // Tier-0 guard below runs only for this case, so ordinary files pay nothing for it.
  bool gate_structural = false;
  const std::span<const uint8_t> stage0 =
      gif ? std::span<const uint8_t>(gif_payload)
          : (numeric ? std::span<const uint8_t>(numeric_payload)
                     : (jpeg ? std::span<const uint8_t>(jpeg_payload) : stage_sql));
  if (opt.precomp && preflate_available() && !stage0.empty()) {
    const std::vector<uint8_t> in(stage0.begin(), stage0.end());
    std::vector<uint8_t> payload;
    if (preflate_unpack(in, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), stage0)) {
      gate_structural = payload.size() >= stage0.size() * 2;
      precomp_payload = std::move(payload);
      precomp = true;
    }
  }

  // Layer 0b — gzip. precomp reaches ZIP and PDF; a gzip member is the same
  // DEFLATE problem wearing a 10-byte header, and .tar.gz is the most common
  // container there is. Tried only when precomp did not fire, because the two are
  // alternatives: a file is either one or the other.
  bool gzip = false;
  if (!precomp && opt.precomp && gzip_available() && !stage0.empty()) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> in(stage0.begin(), stage0.end());
    if (gzip_unpack(in, payload) && precomp_helps(std::span<const uint8_t>(payload), stage0)) {
      gate_structural = payload.size() >= stage0.size() * 2;
      precomp_payload = std::move(payload);
      gzip = true;
    }
  }

  // Layer 0c — xz. The same shape as gzip: a container the block loop cannot see into, so it
  // is inflated and the stream is rebuilt from a recipe read out of its own block header.
  // Tried only when neither precomp nor gzip fired, because all three are alternatives — a
  // file is one of them, not several.
  bool xz = false;
  if (!precomp && !gzip && opt.precomp && xz_available() && !stage0.empty()) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> in(stage0.begin(), stage0.end());
    if (xz_likely(in) && xz_unpack(in, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), stage0)) {
      gate_structural = payload.size() >= stage0.size() * 2;
      precomp_payload = std::move(payload);
      xz = true;
    }
  }

  // Layer 0d — brotli. The same shape again: a container the block loop cannot see into. It has
  // no magic number, so `brotli_unpack` tries to decode and refuses unless the stream rebuilds
  // byte for byte — a wrong guess costs time, never data.
  bool brotli = false;
  if (!precomp && !gzip && !xz && opt.precomp && brotli_available() && !stage0.empty()) {
    std::vector<uint8_t> payload;
    const std::vector<uint8_t> in(stage0.begin(), stage0.end());
    if (brotli_unpack(in, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), stage0)) {
      gate_structural = payload.size() >= stage0.size() * 2;
      precomp_payload = std::move(payload);
      brotli = true;
    }
  }

  // Layer 0 — recursion. A container inside a container is the common case: a
  // .tar.gz holding an .xlsx, a ZIP holding a CSV, a PDF holding a ZIP. The chain
  // above runs once, so it stops at the first boundary and the transform that
  // would have helped never sees the data. Here we re-enter detection on what
  // precomp inflated. Measured on a tar.gz > xlsx: reaching the sheet XML inside
  // was -50.8% on its own, and -89.4% once the numeric transform could reach it.
  // Second container level. gzip hands back a tar and the tar holds a ZIP, so
  // re-entering only the numeric transforms leaves the .xlsx inside a .tar.gz
  // invisible — which is the difference between the 30% gzip buys and the 89% the
  // chain can reach. Runs BEFORE the nested numeric stage, so decode undoes it
  // after.
  bool precomp_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.precomp && preflate_available() &&
      !precomp_payload.empty()) {
    std::vector<uint8_t> payload;
    if (preflate_unpack(precomp_payload, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), precomp_payload)) {
      precomp_payload = std::move(payload);
      precomp_nested = true;
    }
  }

  // base64 one level down. A `.json.gz` is the common shape: gzip inflates to a
  // document whose payload is base64, and until now the nested stage only tried the
  // numeric and XML transforms, so the blob was never decoded.
  bool base64_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.base64 && base64_available() && !precomp_payload.empty()) {
    std::vector<uint8_t> nested;
    if (base64_pack(precomp_payload, nested)) {
      std::vector<uint8_t> check;
      if (base64_unpack(nested, check) && check == precomp_payload &&
          base64_pays(nested, precomp_payload, opt)) {
        precomp_payload = std::move(nested);
        base64_nested = true;
      }
    }
  }

  // Image transforms one level down. Once base64 has decoded a `.json.gz` back to
  // the blob it wrapped, the blob sits at offset 0 and brunsli can recognise it —
  // which is the difference between 0.7% and 19.5% on that file class.
  bool jpeg_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.jpeg && jpeg_available() && !precomp_payload.empty()) {
    std::vector<uint8_t> packed;
    if (jpeg_pack(precomp_payload, packed)) {
      std::vector<uint8_t> check;
      if (jpeg_unpack(packed, check) && check == precomp_payload) {
        precomp_payload = std::move(packed);
        jpeg_nested = true;
      }
    }
  }
  // BMP one level down. A ZIP of BMPs stores raw pixels inside two DEFLATE layers, and the
  // colour-plane split takes it from 120,878 to 1,206 bytes on the fixture that motivated it.
  bool bmp_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.bmp && bmp_available() && !precomp_payload.empty() &&
      bmp_likely(std::span<const uint8_t>(precomp_payload))) {
    std::vector<uint8_t> payload;
    if (bmp_pack(precomp_payload, payload)) {
      std::vector<uint8_t> check;
      if (bmp_unpack(payload, check) && check == precomp_payload &&
          precomp_helps(payload, precomp_payload)) {
        precomp_payload = std::move(payload);
        bmp_nested = true;
      }
    }
  }

  // PNG one level down. A ZIP of PNGs is the common case: precomp inflates the member and
  // hands us the PNG bytes, and without this stage the PNG's own DEFLATE is compressed as
  // if it were opaque — a second compression layer nothing removes. Measured at -79.9%.
  bool png_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.precomp && png_available() && !precomp_payload.empty() &&
      png_likely(precomp_payload)) {
    std::vector<uint8_t> payload;
    if (png_unpack(precomp_payload, payload)) {
      std::vector<uint8_t> check;
      if (png_repack(payload, check) && check.size() == precomp_payload.size() &&
          std::memcmp(check.data(), precomp_payload.data(), precomp_payload.size()) == 0 &&
          precomp_helps(payload, precomp_payload)) {
        precomp_payload = std::move(payload);
        png_nested = true;
      }
    }
  }

  bool gif_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.gif && gif_available() && !precomp_payload.empty()) {
    std::vector<uint8_t> packed;
    if (gif_pack(precomp_payload, packed)) {
      std::vector<uint8_t> check;
      if (gif_unpack(packed, check) && check == precomp_payload) {
        precomp_payload = std::move(packed);
        gif_nested = true;
      }
    }
  }

  bool numeric_nested = false;
  bool xml_numeric_nested = false;
  if ((precomp || gzip || xz || brotli) && opt.numeric && numeric_available() && !precomp_payload.empty()) {
    std::vector<uint8_t> nested;
    // This is the case the whole item exists for: precomp inflated the xlsx and
    // what came out is XML, where the cell values live inside <v> tags.
    if (xml_numeric_available() && xml_numeric_pack(precomp_payload, nested)) {
      std::vector<uint8_t> check;
      if (xml_numeric_unpack(nested, check) && check == precomp_payload) {
        precomp_payload = std::move(nested);
        xml_numeric_nested = true;
      }
    } else if (numeric_pack(precomp_payload, nested)) {
      std::vector<uint8_t> check;
      if (numeric_unpack(nested, check) && check == precomp_payload) {
        precomp_payload = std::move(nested);
        numeric_nested = true;
      }
    }
  }

  // Every Layer 0 bit the stream carries, built once so the three encode paths
  // cannot disagree about which transforms were applied.
  const uint8_t layers3 = (bmp_nested ? kLayer3BmpNested : 0) | (xz ? kLayer3Xz : 0) |
                          (brotli ? kLayer3Brotli : 0);
  const uint8_t layers2 = (sqlite ? kLayer2Sqlite : 0) |
                          (base64_nested ? kLayer2Base64Nested : 0) |
                          (jpeg_nested ? kLayer2JpegNested : 0) |
                          (gif_nested ? kLayer2GifNested : 0) |
                          (png ? kLayer2Png : 0) | (ecc ? kLayer2Ecc : 0) |
                          (bmp ? kLayer2Bmp : 0) |
                          (png_nested ? kLayer2PngNested : 0);
  const uint8_t layers = (numeric_csv ? kLayerNumeric : 0) |
                         (numeric_nested ? kLayerNumericNested : 0) |
                         (xml_numeric ? kLayerXmlNumeric : 0) |
                         (xml_numeric_nested ? kLayerXmlNested : 0) |
                         (base64 ? kLayerBase64 : 0) |
                         (gzip ? kLayerGzip : 0) |
                         (precomp_nested ? kLayerPrecompNested : 0) |
                         (wav ? kLayerWav : 0);

  // Layer 0: the x86 BCJ transform. Applied to the whole stream before
  // anything else, so the filter's state carries across block boundaries the
  // same way it does inside `xz --x86`. It is exactly invertible, so a wrong
  // guess costs ratio and never correctness.
  std::vector<uint8_t> transformed;
  // Everything downstream codes `data`: the inflated payload when precompression
  // applied, otherwise the original. The flag and the data must agree or the
  // decoder will run the inverse transform on bytes that never had it.
  std::span<const uint8_t> data =
      (precomp || gzip || xz || brotli) ? std::span<const uint8_t>(precomp_payload) : stage0;
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
  if (Err e = encode_native(data, opt, bcj, precomp, dedup, jpeg, gif,
                    layers, layers2, layers3, crc, input.size(), native, local);
      e != Err::Ok) {
    return e;
  }

  // Small-file guarantee: on a small input the block framing is a large fraction of the
  // total, so a transform that genuinely helps the payload can still make the ARCHIVE
  // bigger — measured at 336 bytes for a 1.2 KB PNG with the transform against 309 without.
  // Each gate compares coded payload sizes at its own stage; nothing compared the finished
  // streams. Below this threshold the comparison is cheap enough to always run, and it has
  // to happen before the Tier-1 fallback because that path returns early with a stream of
  // its own.
  constexpr size_t kSmallEnoughToCheck = 2u << 20;
  std::vector<uint8_t> untransformed;
  const bool check_plain = input.size() <= kSmallEnoughToCheck &&
                           (layers != 0 || layers2 != 0 || layers3 != 0);
  if (check_plain) {
    CompressOptions plain_opt = opt;
    plain_opt.precomp = false;
    plain_opt.numeric = false;
    plain_opt.base64 = false;
    plain_opt.wav = false;
    plain_opt.bmp = false;
    plain_opt.ecc = false;
    plain_opt.sqlite = false;
    plain_opt.jpeg = false;
    plain_opt.gif = false;
    CompressStats us;
    if (compress(input, untransformed, plain_opt, &us) != Err::Ok) untransformed.clear();
  }

  // Tier-1 guarantee: if LZMA2 does better on the whole stream, ship that.
  // Comparison is on total stream size, framing included, so the promise is
  // about the bytes the user actually gets.
  if (opt.lzma2_fallback && lzma2_available() && !input.empty()) {
    // Counted on `local`: it is what gets published at the end of this function, so an
    // increment on the caller's struct would be overwritten and read as zero.
    ++local.lzma2_trials;
    if (!lzma2_may_win(data, native.size())) {
      // Declined by the sample trial. `--no-lzma2` removes the candidate entirely; this only
      // says the full run was more than 20% out of reach.
      ++local.lzma2_skipped;
    } else {
    std::vector<uint8_t> fallback;
    const Err e = encode_lzma2_stream(data, opt, bcj, precomp, dedup, jpeg, gif,
                          layers, layers2, layers3, crc, input.size(), fallback);
    if (e == Err::Ok && fallback.size() < native.size()) {
      CompressStats fb;
      fb.blocks = 1;
      fb.lzma2_blocks = 1;
      fb.payload_bytes = fallback.size();
      fb.lzma2_stream = true;
      out = std::move(fallback);
      if (stats) *stats = fb;
      if (check_plain && !untransformed.empty() && untransformed.size() < out.size()) {
        out = std::move(untransformed);
      }
      return Err::Ok;
    }
    // A codec failure is not fatal: the native encoding is still valid, so we
    // fall through rather than failing the whole compression.
    }
  }

  // --max-ratio: context mixing over the WHOLE stream, not per block.
  //
  // A CM model is stateful and long-lived; cutting it at a block boundary throws
  // away everything it learned. Measured on xml: per-block --max-ratio gives
  // 394,185 bytes while one CM pass over the same bytes gives 330,394 — 16% that
  // the block boundaries were costing. Tried last and only kept when smaller,
  // because it is the most expensive thing in the engine.
  if (opt.max_ratio && opt.cm_stream && zpaq_available() && !data.empty()) {
    std::vector<uint8_t> whole(data.begin(), data.end());
    std::vector<uint8_t> packed;
    if (zpaq_compress_block(whole, packed)) {
      ByteWriter cw;
      cw.reserve(packed.size() + 64);
      FileHeader cfh;
      cfh.version = kFormatVersion;
      cfh.flags = header_flags(opt, bcj, precomp, /*parallel=*/false, dedup, jpeg, gif);
      cfh.layers = layers;
      cfh.layers2 = layers2;
      cfh.layers3 = layers3;
      cfh.original_size = input.size();
      cfh.true_size = input.size();
      cfh.block_count = 1;
      cfh.checksum = crc;
      write_file_header(cw, cfh);

      BlockHeader bh;
      bh.mode = Mode::CmZpaq;
      bh.uncompressed_size = data.size();
      bh.compressed_size = packed.size();
      bh.sha256 = sha256(data);
      BlockManifest manifest;
      manifest.add(bh);
      if (bh.compressed_size != packed.size()) return Err::SizeMismatch;
      const uint64_t block_at = cw.size();
      write_block_header(cw, bh);
      cw.bytes(packed);
      write_trailer(cw, 1, manifest.digest(), std::span<const uint64_t>(&block_at, 1));

      if (cw.size() < native.size()) {
        CompressStats cs;
        cs.blocks = 1;
        cs.mode_counts[static_cast<uint8_t>(Mode::CmZpaq) & 15] = 1;
        cs.payload_bytes = cw.size();
        out = cw.take();
        if (stats) *stats = cs;
        return Err::Ok;
      }
    }
  }

  // Tier-0 guarantee: never worse than doing nothing.
  //
  // The Tier-1 fallback above compares against LZMA2 over the bytes AFTER Layer 0, so it
  // cannot notice a Layer 0 transform that made the file harder to compress. That is not
  // hypothetical: with the precomp gate restructured, a `.tar.gz` came out at 50,799 bytes
  // from a 50,414-byte input, and every check in the engine was happy — the fallback was
  // comparing against the transformed bytes, not the original ones.
  //
  // So when the container gate took the structural shortcut, also compress the ORIGINAL bytes
  // with LZMA2 and keep whichever is smaller. Scoping it to that case keeps the guarantee and
  // costs nothing on the ordinary path: the rule is a predictor, the check is a measurement,
  // and this is the one place a predictor can decide alone.
  if (gate_structural) {
    std::vector<uint8_t> plain;
    if (encode_lzma2_stream(input, opt, false, false, false, false, false, 0, 0, 0, crc,
                            input.size(), plain) == Err::Ok &&
        !plain.empty() && plain.size() < out.size()) {
      CompressStats ps;
      ps.blocks = 1;
      ps.lzma2_blocks = 1;
      ps.lzma2_stream = true;
      ps.payload_bytes = plain.size();
      out = std::move(plain);
      if (stats) *stats = ps;
      return Err::Ok;
    }
  }

  out = std::move(native);
  if (check_plain && !untransformed.empty() && untransformed.size() < out.size()) {
    out = std::move(untransformed);
  }
  if (stats) *stats = local;
  return Err::Ok;
}

}  // namespace chce
