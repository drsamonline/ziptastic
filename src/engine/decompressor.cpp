// decompressor.cpp — block loop for decode.
//
// The decoder trusts nothing in the stream: every size is validated against
// what is actually present, and the total output is capped by the declared
// original_size so a fuzzed stream cannot force an unbounded allocation.
#include "decompressor.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

#include "chimera/block.hpp"
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
  ByteReader r(in);

  FileHeader fh;
  if (Err e = read_file_header(r, fh); e != Err::Ok) return e;

  std::vector<uint8_t> dst;
  // Reserve the declared size up front (capped at 256 MiB so a corrupt header
  // cannot force a huge allocation); the block loop still validates every byte
  // it produces against that declaration.
  dst.reserve(static_cast<size_t>(std::min<uint64_t>(fh.original_size, 256ull << 20)));

  uint64_t blocks = 0;

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
      if (Err e = read_block_header(r, blks[i].bh); e != Err::Ok) return e;
      if (blks[i].bh.uncompressed_size > fh.original_size - total) {
        return Err::SizeMismatch;
      }
      total += blks[i].bh.uncompressed_size;
      if (!r.take(static_cast<size_t>(blks[i].bh.compressed_size), blks[i].body)) {
        return Err::Truncated;
      }
    }
    if (!r.at_end()) return Err::TrailingData;
    if (total != fh.original_size) return Err::SizeMismatch;

    std::vector<std::vector<uint8_t>> parts(blks.size());
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (;;) {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= blks.size()) break;
        if (decode_body(blks[i].bh, blks[i].body, parts[i]) != Err::Ok) {
          parts[i].clear();  // signals failure to the join below
          parts[i].shrink_to_fit();
        }
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
      dst.insert(dst.end(), parts[i].begin(), parts[i].end());
    }
    blocks = blks.size();
  } else {
  while (blocks < fh.block_count) {
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

    ++blocks;
  }

  if (!r.at_end()) return Err::TrailingData;
  if (dst.size() != fh.original_size) return Err::SizeMismatch;
  }  // end of the sequential (archive-mode) block loop

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
  if ((fh.flags & kFlagPrecomp) != 0) {
    std::vector<uint8_t> original;
    if (!preflate_repack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
  }

  if ((fh.flags & kFlagGif) != 0) {
    std::vector<uint8_t> original;
    if (!gif_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
  }
  if ((fh.flags & kFlagJpeg) != 0) {
    std::vector<uint8_t> original;
    if (!jpeg_unpack(dst, original)) return Err::Corrupt;
    dst = std::move(original);
  }

  // Final check, after every inverse transform: the bytes must be the ones that
  // were compressed. Without this a transform bug would return plausible-looking
  // wrong data, which is the failure mode a stack of layers invites.
  if (crc32(dst) != fh.checksum) return Err::ChecksumMismatch;

  out = std::move(dst);
  if (report) {
    report->blocks = blocks;
    report->original_size = fh.original_size;
  }
  return Err::Ok;
}

}  // namespace chce
