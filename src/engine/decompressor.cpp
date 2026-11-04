// decompressor.cpp — block loop for decode.
//
// The decoder trusts nothing in the stream: every size is validated against
// what is actually present, and the total output is capped by the declared
// original_size so a fuzzed stream cannot force an unbounded allocation.
#include "decompressor.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include "chimera/block.hpp"
#include "chimera/block_index.hpp"
#include "chimera/sha256.hpp"
#include "stream_locate.hpp"
#include "codecs/lzma2_fallback.hpp"
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
#include "precompressors/jpeg_wrap.hpp"
#include "plugin_host.hpp"
#include "precompressors/preflate_wrap.hpp"

namespace chce {
namespace {

// Decodes one block body (metadata + payload) into `dst`. Later phases add
// one branch per mode here.
Err decode_body(const BlockHeader& hdr, std::span<const uint8_t> body,
                std::vector<uint8_t>& dst) {
  switch (hdr.mode) {
    case Mode::Raw:
      // RAW has no metadata, so the body is the block itself.
      if (body.size() != hdr.uncompressed_size) return Err::SizeMismatch;
      dst.insert(dst.end(), body.begin(), body.end());
      return Err::Ok;

    case Mode::Lzma2Fallback: {
      // body = props_len varint | props | raw LZMA2 payload
      ByteReader r(body);
      uint64_t props_len = 0;
      if (!r.varint(props_len)) return Err::BadVarint;
      if (props_len == 0 || props_len > 16) return Err::BadMode;

      std::span<const uint8_t> props;
      if (!r.take(static_cast<size_t>(props_len), props)) return Err::Truncated;

      std::span<const uint8_t> payload;
      if (!r.take(r.remaining(), payload)) return Err::Truncated;
      if (payload.empty()) return Err::Truncated;

      std::vector<uint8_t> block;
      if (Err e = lzma2_decode(payload, props, hdr.uncompressed_size, block);
          e != Err::Ok) {
        return e;
      }
      dst.insert(dst.end(), block.begin(), block.end());
      return Err::Ok;
    }

    case Mode::LzRaw: {
      ByteReader r(body);
      if (Err e = decode_tokens(r, static_cast<uint32_t>(hdr.uncompressed_size),
                                dst);
          e != Err::Ok) {
        return e;
      }
      if (!r.at_end()) return Err::TrailingData;
      return Err::Ok;
    }

    case Mode::RansCtx: {
      ByteReader r(body);
      if (Err e = decode_tokens_rans(r, static_cast<uint32_t>(hdr.uncompressed_size),
                                     dst);
          e != Err::Ok) {
        return e;
      }
      return Err::Ok;
    }

    // Defined in the format but not yet implemented in this engine build.
    case Mode::HuffStatic: {
      ByteReader r(body);
      if (Err e = huff_static_decode(r, static_cast<uint32_t>(hdr.uncompressed_size), dst);
          e != Err::Ok) {
        return e;
      }
      return Err::Ok;
    }

    case Mode::HuffAdapt: {
      ByteReader r(body);
      if (Err e = huff_adaptive_decode(r, static_cast<uint32_t>(hdr.uncompressed_size), dst);
          e != Err::Ok) {
        return e;
      }
      return Err::Ok;
    }

    case Mode::BwtBsc: {
      ByteReader r(body);
      if (Err e = bsc_decompress_block(body, static_cast<uint32_t>(hdr.uncompressed_size),
                                       dst);
          e != Err::Ok) {
        return e;
      }
      return Err::Ok;
    }

    case Mode::CmZpaq: {
      const std::vector<uint8_t> b(body.begin(), body.end());
      if (!zpaq_decompress_block(b, static_cast<uint32_t>(hdr.uncompressed_size), dst)) {
        return Err::Corrupt;
      }
      return Err::Ok;
    }

    // Defined in the format, not implemented in this build. These must stay
    // separate from any implemented case: grouping CmZpaq with them earlier made
    // three unimplemented modes run the zpaq decoder and report "corrupt"
    // instead of "bad-mode".
    case Mode::Ppmd: {
      ByteReader r(body);
      if (Err e = ppmd_decompress_block(body, static_cast<uint32_t>(hdr.uncompressed_size),
                                        dst);
          e != Err::Ok) {
        return e;
      }
      return Err::Ok;
    }

    case Mode::Plugin: {
      ByteReader pr(body);
      uint64_t name_len = 0;
      if (!pr.varint(name_len) || name_len == 0 || name_len > 256) return Err::Corrupt;
      std::span<const uint8_t> name_span;
      if (!pr.take(static_cast<size_t>(name_len), name_span)) return Err::Truncated;
      const std::string name(reinterpret_cast<const char*>(name_span.data()), name_span.size());
      std::span<const uint8_t> payload;
      if (!pr.take(pr.remaining(), payload)) return Err::Truncated;

      static const std::vector<LoadedPlugin> plugins = load_plugins(default_plugin_dir());
      for (const LoadedPlugin& p : plugins) {
        if (p.name != name) continue;
        const size_t start = dst.size();
        dst.resize(start + hdr.uncompressed_size);
        if (p.api->decompress(payload.data(), payload.size(), dst.data() + start,
                              hdr.uncompressed_size) != 0) {
          dst.resize(start);
          return Err::Corrupt;
        }
        return Err::Ok;
      }
      // The stream needs a plugin this build does not have. Say so rather than
      // reporting a generic corruption.
      return Err::BadMode;
    }

    case Mode::Brotli:
    case Mode::Zstd:
    case Mode::Lz4:
      return Err::BadMode;
  }
  return Err::BadMode;
}

}  // namespace

Err decompress(std::span<const uint8_t> in, std::vector<uint8_t>& out,
               DecompressReport* report) {
  // A self-extracting archive is a launcher stub with the stream appended, so the
  // magic is not at offset 0. Find it instead of refusing the file: an SFX made by
  // us, or by any wrapper that concatenates, decodes without a special case.
  //
  // A false positive costs nothing: the header parse validates magic, version, flags
  // and layers, and the CRC-32 of the rebuilt bytes has to match at the end, so a
  // wrong offset fails as a bad stream rather than producing wrong data.
  size_t start = 0;
  if (!locate_stream(in, start)) return Err::BadMagic;
  std::span<const uint8_t> body = in.subspan(start);

  ByteReader r(body);

  FileHeader fh;
  if (Err e = read_file_header(r, fh); e != Err::Ok) return e;

  // Validate before allocating. A block costs at least kMinBlockHeaderBytes on
  // disk, so the bytes actually in hand are a hard upper bound on how many
  // blocks the stream can contain. Without this, a header declaring a million
  // blocks against a hundred bytes made the decoder allocate for a million
  // before discovering there was nothing there — the desync failures in this
  // project surfaced exactly that way, as a bad_alloc from an unvalidated size
  // field rather than as a rejected stream.
  if (fh.block_count > r.remaining() / kMinBlockHeaderBytes) {
    return Err::BadBlockSize;
  }

  std::vector<uint8_t> dst;
  // Reserve the declared size up front (capped at 256 MiB so a corrupt header
  // cannot force a huge allocation); the block loop still validates every byte
  // it produces against that declaration.
  dst.reserve(static_cast<size_t>(std::min<uint64_t>(fh.original_size, 256ull << 20)));

  uint64_t blocks = 0;
  // Per-block digests are checked as blocks come out; this binds the ordered set
  // of headers so a dropped or reordered block is caught too.
  BlockManifest manifest;
  // Where each block actually started, stream-relative. The v8 trailer carries a table of
  // these, and a decoder that walked the blocks is the only thing that can check it: the
  // table must name the same positions, or it is a lie about a stream that decoded fine.
  std::vector<uint64_t> observed_offsets;
  observed_offsets.reserve(static_cast<size_t>(fh.block_count));

  // Declared block count, so a failure message can say "block 3 of 25" even
  // though the decode never finished. Overwritten with the real count at the end.
  if (report) report->blocks = fh.block_count;

  // Records which block failed before unwinding, so the CLI can say so.
  auto fail_block = [report](uint64_t idx) -> Err {
    if (report) report->bad_block = idx;
    return Err::BlockChecksumMismatch;
  };

  // Parallel streams: every block is self-contained, so each can be decoded into
  // its own buffer and the results concatenated. This is only safe because
  // kFlagParallel guarantees no block references another — in archive mode the
  // same code would read from output the block has not produced yet.
  // Thread startup has to be paid off. Measured on xml: at 82 blocks of 64 KiB
  // this path takes 115 ms against 138 ms sequential (-17%); at 11 blocks of
  // 512 KiB it is SLOWER (356-489 ms against 241-295 ms), because two threads
  // cost more than 11 small blocks save. Below kParallelDecodeMinBlocks the
  // sequential path wins, so it is used instead.
  constexpr uint64_t kParallelDecodeMinBlocks = 8;
  if ((fh.flags & kFlagParallel) != 0 && fh.block_count >= kParallelDecodeMinBlocks) {
    struct Blk {
      BlockHeader bh;
      std::span<const uint8_t> body;
    };
    std::vector<Blk> blks(static_cast<size_t>(fh.block_count));
    uint64_t total = 0;
    for (size_t i = 0; i < blks.size(); ++i) {
      observed_offsets.push_back(r.offset());
      if (Err e = read_block_header(r, blks[i].bh); e != Err::Ok) return e;
      manifest.add(blks[i].bh);
      if (blks[i].bh.uncompressed_size > fh.original_size - total) {
        return Err::SizeMismatch;
      }
      total += blks[i].bh.uncompressed_size;
      if (!r.take(static_cast<size_t>(blks[i].bh.compressed_size), blks[i].body)) {
        return Err::Truncated;
      }
    }
    if (total != fh.original_size) return Err::SizeMismatch;

    std::vector<std::vector<uint8_t>> parts(blks.size());
    // Per-block integrity, decided inside the worker that produced the bytes.
    //
    // The digest used to run in the join loop below, on one thread, after every block had
    // been decoded — so the SHA-256 of the whole stream was serial no matter how many
    // threads decoded it. At 140 MB/s that is about 26 ms of a 3.7 MB decode, and it is
    // most of the difference between this path and `decode_blocks_parallel`, which checks
    // its digests in the workers (FINDINGS.md §3c). 0 = not checked, 1 = ok, 2 = mismatch.
    std::vector<uint8_t> digest_state(blks.size(), 0);
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (;;) {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= blks.size()) break;
        if (decode_body(blks[i].bh, blks[i].body, parts[i]) != Err::Ok) {
          parts[i].clear();  // signals failure to the join below
          parts[i].shrink_to_fit();
          continue;
        }
        digest_state[i] =
            sha256_equal(sha256(parts[i]), blks[i].bh.sha256) ? uint8_t{1} : uint8_t{2};
      }
    };
    unsigned nt = std::thread::hardware_concurrency();
    if (nt == 0) nt = 1;
    if (nt > blks.size()) nt = static_cast<unsigned>(blks.size());
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (unsigned i = 0; i < nt; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    dst.reserve(static_cast<size_t>(total));
    for (size_t i = 0; i < blks.size(); ++i) {
      if (parts[i].size() != blks[i].bh.uncompressed_size) return Err::Corrupt;
      // Integrity before the bytes are concatenated: a bad block is named by its
      // index while the good ones are still separable. The answer was computed in the
      // worker; this only reads it.
      if (digest_state[i] != 1) return fail_block(i);
      dst.insert(dst.end(), parts[i].begin(), parts[i].end());
    }
    blocks = blks.size();
    if (Err e = read_trailer(r, fh.block_count, manifest.digest(), observed_offsets);
        e != Err::Ok) {
      return e;
    }
    if (!r.at_end()) return Err::TrailingData;
  } else {
  while (blocks < fh.block_count) {
    observed_offsets.push_back(r.offset());
    BlockHeader bh;
    if (Err e = read_block_header(r, bh); e != Err::Ok) return e;

    // Never let a block claim more output than the stream declared.
    const uint64_t produced = dst.size();
    if (bh.uncompressed_size > fh.original_size - produced) {
      return Err::SizeMismatch;
    }

    std::span<const uint8_t> body;
    if (!r.take(static_cast<size_t>(bh.compressed_size), body)) {
      return Err::Truncated;
    }
    const size_t before = dst.size();
    if (Err e = decode_body(bh, body, dst); e != Err::Ok) return e;
    if (dst.size() - before != bh.uncompressed_size) return Err::SizeMismatch;
    manifest.add(bh);
    if (!sha256_equal(sha256(std::span<const uint8_t>(dst).subspan(before)),
                      bh.sha256)) {
      return fail_block(blocks);
    }

    ++blocks;
  }

  if (dst.size() != fh.original_size) return Err::SizeMismatch;
  if (Err e = read_trailer(r, fh.block_count, manifest.digest(), observed_offsets);
      e != Err::Ok) {
    return e;
  }
  if (!r.at_end()) return Err::TrailingData;
  }  // end of the sequential (archive-mode) block loop

  // The inverse chain can EXPAND, and until this guard existed the only thing standing
  // between a crafted stream and an out-of-memory kill was the CRC check at the very end.
  // A twenty-byte dedup payload can legitimately describe a terabyte (a self-referencing
  // copy), and the decoder would build it byte by byte before discovering the checksum
  // was wrong.
  //
  // v5 had no field for the true original size — `original_size` is the compressed-domain
  // size — so the only bound was a heuristic ceiling of 4096x. v6 declares the true size,
  // which makes this exact; the ceiling is kept as a named value rather than deleted
  // because the check below reads better with a name.
  // v6 declares the true original size, which makes the FINAL check exact. It does not
  // make the intermediate bound exact, and assuming it did broke the container paths: a
  // `.tar.gz` of 50 KB inflates to a 300 KB tar and then to a larger inflated ZIP before
  // the nested transforms shrink it again. The chain is not monotonic toward the final
  // size — it goes up and comes back down — so intermediates need a ceiling above the
  // final, and only the last comparison can be `==`.
  const uint64_t expansion_cap = fh.true_size * 4096 + (uint64_t{1} << 20);
  auto within_cap = [&](const std::vector<uint8_t>& buf) {
    return static_cast<uint64_t>(buf.size()) <= expansion_cap;
  };

  // Undo the Layer 0 transforms in the reverse order they were applied:
  // BCJ first, then DEFLATE recompression.
  if ((fh.flags & kFlagBcj) != 0) {
    bcj_x86_transform(dst, false);
  }
  if ((fh.flags & kFlagDedup) != 0) {
    std::vector<uint8_t> expanded;
    if (!dedup_unpack(dst, expanded)) return Err::Corrupt;
    dst = std::move(expanded);
  }
  // Recursion: the nested numeric transform was applied AFTER precomp on encode,
  // so it is undone BEFORE it here. Order is the whole correctness argument.
  if ((fh.layers & kLayerNumericNested) != 0) {
    std::vector<uint8_t> original;
    if (!numeric_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers & kLayerXmlNested) != 0) {
    std::vector<uint8_t> original;
    if (!xml_numeric_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // Image transforms one level down were applied last of the nested stages, so they
  // are undone first among them.
  if ((fh.layers2 & kLayer2GifNested) != 0) {
    std::vector<uint8_t> original;
    if (!gif_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2JpegNested) != 0) {
    std::vector<uint8_t> original;
    if (!jpeg_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // PNG one level down was applied after the nested base64 and before the image stages,
  // so it is undone after them.
  if ((fh.layers3 & kLayer3BmpNested) != 0) {
    std::vector<uint8_t> original;
    if (!bmp_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2PngNested) != 0) {
    std::vector<uint8_t> original;
    if (!png_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // base64 one level down was applied after the second container level and before
  // the nested numeric transforms, so it is undone between them.
  if ((fh.layers2 & kLayer2Base64Nested) != 0) {
    std::vector<uint8_t> original;
    if (!base64_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // The second container level was applied before the nested numeric transforms,
  // so it is undone after them and before the container that held it.
  if ((fh.layers & kLayerPrecompNested) != 0) {
    std::vector<uint8_t> original;
    if (!preflate_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // gzip and precomp are alternatives, both undone after the nested transforms
  // that ran on what they inflated.
  if ((fh.layers & kLayerGzip) != 0) {
    std::vector<uint8_t> original;
    if (!gzip_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.flags & kFlagPrecomp) != 0) {
    std::vector<uint8_t> original;
    if (!preflate_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }

  if ((fh.layers & kLayerNumeric) != 0) {
    std::vector<uint8_t> original;
    if (!numeric_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers & kLayerXmlNumeric) != 0) {
    std::vector<uint8_t> original;
    if (!xml_numeric_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.flags & kFlagGif) != 0) {
    std::vector<uint8_t> original;
    if (!gif_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.flags & kFlagJpeg) != 0) {
    std::vector<uint8_t> original;
    if (!jpeg_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2Bmp) != 0) {
    std::vector<uint8_t> original;
    if (!bmp_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2Ecc) != 0) {
    std::vector<uint8_t> original;
    if (!ecc_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2Png) != 0) {
    std::vector<uint8_t> original;
    if (!png_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers2 & kLayer2Sqlite) != 0) {
    std::vector<uint8_t> original;
    if (!sqlite_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  if ((fh.layers & kLayerWav) != 0) {
    std::vector<uint8_t> original;
    if (!wav_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }
  // base64 was applied FIRST on encode, so it is undone LAST: everything above
  // this line was working on the decoded bytes.
  if ((fh.layers & kLayerBase64) != 0) {
    std::vector<uint8_t> original;
    if (!base64_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
    if (!within_cap(dst)) return Err::SizeMismatch;
  }

  // Final check, after every inverse transform: the bytes must be the ones that
  // were compressed. Without this a transform bug would return plausible-looking
  // wrong data, which is the failure mode a stack of layers invites.
  // Exact, because the header now says what the answer must be.
  if (dst.size() != fh.true_size) return Err::SizeMismatch;
  if (crc32(dst) != fh.checksum) return Err::ChecksumMismatch;

  out = std::move(dst);
  if (report) {
    report->blocks = blocks;
    // The size the CALLER gets, not the size the header declares. Those differ whenever
    // a Layer 0 transform ran, because the header counts the compressed-domain payload
    // and the output is what the inverse chain rebuilt. Reporting the header value made
    // `DecompressReport::original_size` disagree with the bytes in `out` for every
    // transformed stream — found by fuzzing streams built with precomp on, which the
    // original fuzz test never did (it used default options, where precomp is false).
    report->original_size = out.size();
  }
  return Err::Ok;
}

namespace {

// The two refusals every block-addressed request shares: the index must describe this
// stream, and the stream's blocks must mean something on their own.
Err check_block_request(const StreamIndex& index) {
  const FileHeader& fh = index.header;
  if (index.size() != fh.block_count) return Err::Corrupt;  // an index of another stream
  // Both halves of "a block's bytes mean something on their own": it must not reference
  // another block, and nothing outside the block loop may have changed the payload.
  if (!blocks_are_independently_decodable(fh) || !stream_is_untransformed(fh)) {
    return Err::NotSelfContained;
  }
  return Err::Ok;
}

// Decodes blocks [first, last] and appends. No whole-stream check: that belongs to the
// caller that knows whether the range it asked for is the whole stream.
Err decode_range(std::span<const uint8_t> in, const StreamIndex& index, uint64_t first,
                 uint64_t last, std::vector<uint8_t>& out, uint64_t* bad_block) {
  for (uint64_t i = first; i <= last; ++i) {
    const BlockHeader& bh = index.blocks[static_cast<size_t>(i)];
    const uint64_t body_at = index.body_offsets[static_cast<size_t>(i)];
    if (body_at > in.size() || bh.compressed_size > in.size() - body_at) {
      return Err::Truncated;
    }
    const std::span<const uint8_t> body =
        in.subspan(static_cast<size_t>(body_at), static_cast<size_t>(bh.compressed_size));

    std::vector<uint8_t> part;
    part.reserve(static_cast<size_t>(bh.uncompressed_size));
    if (Err e = decode_body(bh, body, part); e != Err::Ok) return e;
    if (part.size() != bh.uncompressed_size) return Err::SizeMismatch;
    // The digest before the bytes, exactly as the walking decoder does it: a damaged
    // block is named and contributes nothing.
    if (!sha256_equal(sha256(part), bh.sha256)) {
      if (bad_block != nullptr) *bad_block = i;
      return Err::BlockChecksumMismatch;
    }
    out.insert(out.end(), part.begin(), part.end());
  }
  return Err::Ok;
}

// The whole-stream promise, made only by a decode that covered every block: the exact
// length and the CRC-32 of the bytes the caller asked for.
Err check_whole_stream(const StreamIndex& index, const std::vector<uint8_t>& out) {
  if (static_cast<uint64_t>(out.size()) != index.header.true_size) return Err::SizeMismatch;
  if (crc32(out) != index.header.checksum) return Err::ChecksumMismatch;
  return Err::Ok;
}

}  // namespace

// Reaching one block, or a run of them, without walking the stream from the front. See
// include/chimera/block_index.hpp for what this is for and what it refuses.
Err decode_blocks(std::span<const uint8_t> in, const StreamIndex& index, uint64_t first,
                  uint64_t last, std::vector<uint8_t>& out, uint64_t* bad_block) {
  if (Err e = check_block_request(index); e != Err::Ok) return e;
  if (first > last || last >= index.header.block_count) return Err::BadBlockSize;

  if (Err e = decode_range(in, index, first, last, out, bad_block); e != Err::Ok) return e;

  if (first == 0 && last + 1 == index.header.block_count) {
    return check_whole_stream(index, out);
  }
  return Err::Ok;
}

Err decode_blocks_parallel(std::span<const uint8_t> in, const StreamIndex& index,
                           unsigned threads, std::vector<uint8_t>& out,
                           uint64_t* bad_block) {
  if (Err e = check_block_request(index); e != Err::Ok) return e;
  const uint64_t count = index.header.block_count;
  if (count == 0) return check_whole_stream(index, out);

  if (threads < 2 || count < 2) return decode_blocks(in, index, 0, count - 1, out, bad_block);
  if (threads > count) threads = static_cast<unsigned>(count);

  // Contiguous ranges, one per thread: the blocks are independent, so the split can be
  // anywhere, and contiguous keeps each thread's output in stream order without a sort.
  std::vector<std::vector<uint8_t>> parts(threads);
  std::vector<Err> errors(threads, Err::Ok);
  std::vector<uint64_t> bad(threads, kNoBlock);
  {
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
      pool.emplace_back([&, t]() {
        const uint64_t lo = count * t / threads;
        const uint64_t hi = count * (t + 1) / threads;
        if (lo >= hi) return;
        errors[t] = decode_range(in, index, lo, hi - 1, parts[t], &bad[t]);
      });
    }
    for (std::thread& th : pool) th.join();
  }

  for (unsigned t = 0; t < threads; ++t) {
    if (errors[t] == Err::Ok) continue;
    if (bad_block != nullptr && bad[t] != kNoBlock) *bad_block = bad[t];
    return errors[t];
  }

  out.reserve(static_cast<size_t>(index.header.true_size));
  for (std::vector<uint8_t>& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
    part.clear();
    part.shrink_to_fit();
  }
  return check_whole_stream(index, out);
}

}  // namespace chce
