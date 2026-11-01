// compressor.cpp — orchestrates the per-block encode loop and the Tier-1
// whole-stream LZMA2 fallback.
#include "compressor.hpp"

#include <algorithm>
#include <atomic>
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
                  bool bcj, bool precomp, bool dedup, bool jpeg, bool gif,
                  uint8_t layers, uint32_t checksum,
                  std::vector<uint8_t>& out, CompressStats& stats) {
  const size_t block_count =
      input.empty() ? 0 : (input.size() + opt.block_size - 1) / opt.block_size;
  const bool parallel = opt.parallel && block_count > 1;

  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = header_flags(opt, bcj, precomp, parallel, dedup, jpeg, gif);
  fh.layers = layers;
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

  // Every block is sealed the same way: digest its plaintext, then record the
  // header in the manifest. One helper so the four write sites cannot disagree
  // about what a block's hash covers.
  BlockManifest manifest;
  auto seal = [&manifest](BlockHeader& h, std::span<const uint8_t> plaintext) {
    h.sha256 = sha256(plaintext);
    manifest.add(h);
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

    for (size_t b = 0; b < nblocks; ++b) {
      const size_t pos = b * opt.block_size;
      const size_t n = std::min(opt.block_size, input.size() - pos);
      seal(encoded[b].hdr, input.subspan(pos, n));
      write_block_header(w, encoded[b].hdr);
      w.bytes(encoded[b].body);
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
      write_block_header(w, best.hdr);
      w.bytes(best.body);
      account(best);
      pos += n;
    }
  }

  write_trailer(w, fh.block_count, manifest.digest());
  out = w.take();
  return Err::Ok;
}

// The Tier-1 fallback: the whole input as a single raw LZMA2 stream inside a
// single LZMA2_FALLBACK block. Block body layout (docs/FORMAT.md):
//   props_len varint | props bytes | raw LZMA2 payload
Err encode_lzma2_stream(std::span<const uint8_t> input,
                        const CompressOptions& opt, bool bcj, bool precomp,
                        bool dedup, bool jpeg, bool gif, uint8_t layers,
                        uint32_t checksum, std::vector<uint8_t>& out) {
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
  fh.original_size = input.size();
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
  write_block_header(w, bh);
  w.bytes(body.span());
  write_trailer(w, 1, manifest.digest());

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

  // Layer 0 — integer CSV/TSV. Runs first: it is the only transform that needs to
  // see the raw text, and a CSV is not DEFLATE, not x86, not an image.
  std::vector<uint8_t> numeric_payload;
  // `numeric` means numeric_payload holds the stage-0 payload, whichever transform
  // produced it: the downstream stages pick the payload on this flag, so it has to
  // cover both variants or they code the wrong bytes while the layer bit claims a
  // transform ran. `numeric_csv` is only for the layer bit itself.
  bool numeric = false;
  bool numeric_csv = false;
  bool xml_numeric = false;
  if (opt.numeric && numeric_available() && !input.empty()) {
    std::vector<uint8_t> packed;
    // Spreadsheet XML first: an .xlsx that precomp never inflated still shows up
    // here as a bare sheet1.xml, and the CSV transform cannot see inside <v> tags.
    if (xml_numeric_available() && xml_numeric_pack(input, packed)) {
      std::vector<uint8_t> check;
      if (xml_numeric_unpack(packed, check) && check.size() == input.size() &&
          std::memcmp(check.data(), input.data(), input.size()) == 0) {
        numeric_payload = std::move(packed);
        numeric = true;
        xml_numeric = true;
      }
    } else if (numeric_pack(input, packed)) {
      std::vector<uint8_t> check;
      if (numeric_unpack(packed, check) && check.size() == input.size() &&
          std::memcmp(check.data(), input.data(), input.size()) == 0) {
        numeric_payload = std::move(packed);
        numeric = true;
        numeric_csv = true;
      }
    }
  }

  // Layer 0 — JPEG. First, because a JPEG is not DEFLATE, not x86 code and not
  // text, so every other layer would waste its time on it. brunsli re-encodes
  // the DCT coefficients and keeps a recipe that reproduces the file exactly;
  // verified here before it is trusted, like every other transform.
  std::vector<uint8_t> jpeg_payload;
  bool jpeg = false;
  if (opt.jpeg && jpeg_available() && !input.empty()) {
    const std::vector<uint8_t> in(input.begin(), input.end());
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
                : (jpeg ? std::span<const uint8_t>(jpeg_payload) : input);
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
  const std::span<const uint8_t> stage0 =
      gif ? std::span<const uint8_t>(gif_payload)
          : (numeric ? std::span<const uint8_t>(numeric_payload)
                     : (jpeg ? std::span<const uint8_t>(jpeg_payload) : input));
  if (opt.precomp && preflate_available() && !stage0.empty()) {
    const std::vector<uint8_t> in(stage0.begin(), stage0.end());
    std::vector<uint8_t> payload;
    if (preflate_unpack(in, payload) &&
        precomp_helps(std::span<const uint8_t>(payload), stage0)) {
      precomp_payload = std::move(payload);
      precomp = true;
    }
  }

  // Layer 0 — recursion. A container inside a container is the common case: a
  // .tar.gz holding an .xlsx, a ZIP holding a CSV, a PDF holding a ZIP. The chain
  // above runs once, so it stops at the first boundary and the transform that
  // would have helped never sees the data. Here we re-enter detection on what
  // precomp inflated. Measured on a tar.gz > xlsx: reaching the sheet XML inside
  // was -50.8% on its own, and -89.4% once the numeric transform could reach it.
  bool numeric_nested = false;
  bool xml_numeric_nested = false;
  if (precomp && opt.numeric && numeric_available() && !precomp_payload.empty()) {
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
  const uint8_t layers = (numeric_csv ? kLayerNumeric : 0) |
                         (numeric_nested ? kLayerNumericNested : 0) |
                         (xml_numeric ? kLayerXmlNumeric : 0) |
                         (xml_numeric_nested ? kLayerXmlNested : 0);

  // Layer 0: the x86 BCJ transform. Applied to the whole stream before
  // anything else, so the filter's state carries across block boundaries the
  // same way it does inside `xz --x86`. It is exactly invertible, so a wrong
  // guess costs ratio and never correctness.
  std::vector<uint8_t> transformed;
  // Everything downstream codes `data`: the inflated payload when precompression
  // applied, otherwise the original. The flag and the data must agree or the
  // decoder will run the inverse transform on bytes that never had it.
  std::span<const uint8_t> data =
      precomp ? std::span<const uint8_t>(precomp_payload) : stage0;
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
                    layers, crc, native, local); e != Err::Ok) {
    return e;
  }

  // Tier-1 guarantee: if LZMA2 does better on the whole stream, ship that.
  // Comparison is on total stream size, framing included, so the promise is
  // about the bytes the user actually gets.
  if (opt.lzma2_fallback && lzma2_available() && !input.empty()) {
    std::vector<uint8_t> fallback;
    const Err e = encode_lzma2_stream(data, opt, bcj, precomp, dedup, jpeg, gif,
                          layers, crc, fallback);
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

  // --max-ratio: context mixing over the WHOLE stream, not per block.
  //
  // A CM model is stateful and long-lived; cutting it at a block boundary throws
  // away everything it learned. Measured on xml: per-block --max-ratio gives
  // 394,185 bytes while one CM pass over the same bytes gives 330,394 — 16% that
  // the block boundaries were costing. Tried last and only kept when smaller,
  // because it is the most expensive thing in the engine.
  if (opt.max_ratio && zpaq_available() && !data.empty()) {
    std::vector<uint8_t> whole(data.begin(), data.end());
    std::vector<uint8_t> packed;
    if (zpaq_compress_block(whole, packed)) {
      ByteWriter cw;
      cw.reserve(packed.size() + 64);
      FileHeader cfh;
      cfh.version = kFormatVersion;
      cfh.flags = header_flags(opt, bcj, precomp, /*parallel=*/false, dedup, jpeg, gif);
      cfh.layers = layers;
      cfh.original_size = input.size();
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
      write_block_header(cw, bh);
      cw.bytes(packed);
      write_trailer(cw, 1, manifest.digest());

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

  out = std::move(native);
  if (stats) *stats = local;
  return Err::Ok;
}

}  // namespace chce
