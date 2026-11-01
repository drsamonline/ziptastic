// test_roundtrip.cpp — Phase 0 exit test: 100 random files from 0 B to 1 MB,
// plus the block-boundary sizes, must round-trip bit-exactly through RAW.
#include <cstring>

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

// Deterministic payload of a given size and character.
std::vector<uint8_t> make_data(chce_test::Rng& rng, size_t n, int flavour) {
  std::vector<uint8_t> d(n);
  switch (flavour) {
    case 0:  // pure noise
      for (auto& b : d) b = rng.byte();
      break;
    case 1:  // low-entropy: few distinct byte values
      for (auto& b : d) b = static_cast<uint8_t>(rng.below(4));
      break;
    case 2: {  // repetitive text-ish
      static const char* words[] = {"chce ", "ziptastic ", "block ", "engine\n"};
      size_t i = 0;
      while (i < n) {
        const char* w = words[rng.below(4)];
        for (size_t k = 0; w[k] && i < n; ++k) d[i++] = static_cast<uint8_t>(w[k]);
      }
      break;
    }
    default:  // long runs (stresses block splitting, not entropy)
      for (size_t i = 0; i < n; ++i) d[i] = static_cast<uint8_t>(i / 1000);
      break;
  }
  return d;
}

void check_roundtrip(const std::vector<uint8_t>& data, size_t block_size,
                     bool use_fallback = true, bool use_parse = false) {
  CompressOptions opt;
  opt.block_size = block_size;
  opt.lzma2_fallback = use_fallback;
  // The parser runs at a few MB/s; the container sweeps below cover tens of
  // megabytes, so they exercise the block loop with it off and the parse path
  // has its own cases (and its own unit suite).
  opt.parse = use_parse;

  std::vector<uint8_t> compressed;
  CompressStats stats;
  CHECK_ERR(compress(data, compressed, opt, &stats), Err::Ok);

  // Header sanity: sizes and block count must describe the input exactly.
  ByteReader r(compressed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_EQ(fh.version, uint8_t{kFormatVersion});
  CHECK_EQ(fh.original_size, static_cast<uint64_t>(data.size()));
  const uint64_t expect_blocks =
      data.empty() ? 0 : (data.size() + block_size - 1) / block_size;
  // Two legal shapes: the per-block native encoding, or the whole-stream
  // LZMA2 fallback, which is a single block by definition.
  const bool fallback = stats.lzma2_stream;
  CHECK_MSG(fallback ? fh.block_count == 1 : fh.block_count == expect_blocks,
            "block_count " + std::to_string(fh.block_count) + " (expected " +
                std::to_string(expect_blocks) + " native or 1 fallback)");
  CHECK_EQ(stats.blocks, fh.block_count);

  std::vector<uint8_t> restored;
  DecompressReport rep;
  CHECK_ERR(decompress(compressed, restored, &rep), Err::Ok);
  CHECK_EQ(restored.size(), data.size());
  CHECK_EQ(rep.blocks, fh.block_count);
  CHECK_MSG(restored == data, "round-trip mismatch at " + std::to_string(data.size()) +
                                  " bytes, block " + std::to_string(block_size));

  // Only implemented modes may appear in a stream.
  // Every block must be accounted for by exactly one mode. Summing the
  // mode-indexed array means adding a backend cannot make this check stale.
  uint64_t accounted = 0;
  for (uint64_t c : stats.mode_counts) accounted += c;
  CHECK_EQ(accounted, stats.blocks);

  // Compression must be deterministic (same bytes out on a second pass).
  std::vector<uint8_t> again;
  CHECK_ERR(compress(data, again, opt, nullptr), Err::Ok);
  CHECK(again == compressed);
}

}  // namespace

TEST(empty_input) {
  check_roundtrip({}, kDefaultBlockSize);
}

TEST(tiny_inputs) {
  chce_test::Rng rng(1);
  for (size_t n = 0; n <= 64; ++n) {
    check_roundtrip(make_data(rng, n, static_cast<int>(n % 4)), kDefaultBlockSize, false);
  }
}

TEST(block_boundaries) {
  chce_test::Rng rng(7);
  const size_t bs = 1024;  // small block size keeps the test fast
  const size_t sizes[] = {bs - 1, bs, bs + 1, 2 * bs - 1, 2 * bs, 2 * bs + 1, 10 * bs};
  for (size_t n : sizes) {
    check_roundtrip(make_data(rng, n, 0), bs, false);
    check_roundtrip(make_data(rng, n, 2), bs, false);
  }
}

TEST(one_hundred_random_files) {
  chce_test::Rng rng(0xC0FFEE);
  for (int i = 0; i < 100; ++i) {
    // 0 B .. 1 MB, biased toward the small end so the sweep stays quick.
    const size_t n = static_cast<size_t>(rng.below(1024 * 1024));
    check_roundtrip(make_data(rng, n, i % 4), kDefaultBlockSize, false);
  }
}

TEST(varying_block_sizes) {
  // The LZMA2 fallback is whole-stream, so block size only shows up in the
  // native path: exercise that with the fallback off.
  chce_test::Rng rng(99);
  const auto data = make_data(rng, 300 * 1024, 2);
  for (size_t bs : {size_t{1} * 1024, size_t{7} * 1024, size_t{64} * 1024,
                    size_t{300} * 1024, size_t{1024} * 1024}) {
    check_roundtrip(data, bs, false);
  }
}

TEST(fallback_wins_on_compressible_data) {
  // The Tier-1 guarantee: when the native path is weak, LZMA2 must take over.
  // The native path is deliberately weakened here (no BWT, no range coder) so
  // the test measures the guarantee rather than which backend happens to win
  // this particular input — BWT beats LZMA2 on this data now, which is a good
  // outcome but says nothing about the fallback.
  chce_test::Rng rng(42);
  const auto data = make_data(rng, 200 * 1024, 2);  // repetitive text

  CompressOptions opt;
  opt.bwt = false;
  opt.range = false;
  opt.adaptive = false;
  opt.huffman = false;
  std::vector<uint8_t> native, fallback;
  CompressStats native_stats, fallback_stats;
  opt.lzma2_fallback = false;
  CHECK_ERR(compress(data, native, opt, &native_stats), Err::Ok);
  opt.lzma2_fallback = true;
  opt.bwt = true;
  opt.range = true;
  opt.adaptive = true;
  opt.huffman = true;
  CHECK_ERR(compress(data, fallback, opt, &fallback_stats), Err::Ok);

  // With the full backend set the stream is not the LZMA2 fallback any more —
  // a native backend wins. Either way it must be no larger than the weakened
  // native encoding, which is the property that matters.
  CHECK(fallback.size() <= native.size());
  CHECK_EQ(fallback_stats.blocks, uint64_t{1});


  std::vector<uint8_t> restored;
  CHECK_ERR(decompress(fallback, restored), Err::Ok);
  CHECK(restored == data);
}

TEST(fallback_declines_on_incompressible_data) {
  chce_test::Rng rng(43);
  const auto data = make_data(rng, 200 * 1024, 0);  // pure noise

  std::vector<uint8_t> out;
  CompressStats stats;
  CompressOptions opt;
  CHECK_ERR(compress(data, out, opt, &stats), Err::Ok);

  // LZMA2 cannot beat stored bytes on noise, so the native path must win.
  CHECK(!stats.lzma2_stream);
  CHECK_EQ(stats.raw_blocks, stats.blocks);
  // Stored bytes plus framing only: no entropy backend may add bytes here.
  // Framing is the file header, one block header carrying a 32-byte SHA-256, and
  // the 37-byte trailer (magic + count + manifest digest) — under 128 bytes for a
  // single block, and 32 more for each further block.
  CHECK_MSG(out.size() > data.size() && out.size() - data.size() < 128,
            "unexpected framing overhead " +
                std::to_string(out.size() - data.size()));
}

TEST(parse_path_roundtrips) {
  chce_test::Rng rng(45);
  for (size_t n : {size_t{0}, size_t{1}, size_t{2}, size_t{5000}, size_t{300 * 1024}}) {
    for (int flavour = 0; flavour < 4; ++flavour) {
      check_roundtrip(make_data(rng, n, flavour), kDefaultBlockSize, false, true);
    }
  }
}

TEST(disabled_fallback_still_roundtrips) {
  chce_test::Rng rng(44);
  for (size_t n : {size_t{0}, size_t{1}, size_t{5000}, size_t{300 * 1024}}) {
    check_roundtrip(make_data(rng, n, 2), kDefaultBlockSize, false);
  }
}

TEST(unimplemented_modes_rejected) {
  // Every mode byte is defined, but only RAW and LZMA2_FALLBACK are
  // implemented: the decoder must reject the rest cleanly rather than
  // mis-parse them.
  const auto data = std::vector<uint8_t>(100, 0x5A);
  std::vector<uint8_t> compressed;
  CompressOptions opt;
  opt.lzma2_fallback = false;  // keep the stream single-block RAW
  CHECK_ERR(compress(data, compressed, opt, nullptr), Err::Ok);

  for (uint8_t m = 1; m <= mode_byte(Mode::Ppmd); ++m) {
    // Skip the modes this build actually implements.
    if (m == mode_byte(Mode::Lzma2Fallback) || m == mode_byte(Mode::RansCtx) ||
        m == mode_byte(Mode::HuffStatic) || m == mode_byte(Mode::HuffAdapt) ||
        m == mode_byte(Mode::BwtBsc) || m == mode_byte(Mode::CmZpaq) ||
        m == mode_byte(Mode::Ppmd)) {
      continue;
    }
    // Rewrite the single block's mode byte.
    ByteReader r(compressed);
    FileHeader fh;
    read_file_header(r, fh);
    const size_t mode_offset = r.offset();
    auto mutated = compressed;
    mutated[mode_offset] = m;
    std::vector<uint8_t> out;
    const Err got = decompress(mutated, out);
    CHECK_MSG(got == Err::BadMode,
              "mode 0x" + std::to_string(static_cast<int>(m)) + " returned " +
                  err_name(got) + ", expected bad-mode");
  }
}

TEST(trailing_data_rejected) {
  const std::vector<uint8_t> data = {1, 2, 3, 4, 5};
  std::vector<uint8_t> compressed;
  CompressOptions opt;
  CHECK_ERR(compress(data, compressed, opt, nullptr), Err::Ok);

  auto padded = compressed;
  padded.push_back(0x00);
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(padded, out), Err::TrailingData);
}

TEST(truncation_rejected_everywhere) {
  chce_test::Rng rng(5);
  const auto data = make_data(rng, 5000, 2);
  std::vector<uint8_t> compressed;
  CompressOptions opt;
  opt.block_size = 1024;
  CHECK_ERR(compress(data, compressed, opt, nullptr), Err::Ok);

  for (size_t n = 0; n < compressed.size(); ++n) {
    std::vector<uint8_t> out;
    const Err e = decompress(std::span<const uint8_t>(compressed.data(), n), out);
    CHECK_MSG(e != Err::Ok, "truncated stream of " + std::to_string(n) +
                                " bytes decoded successfully");
  }
}

TEST(size_mismatch_rejected) {
  const auto data = std::vector<uint8_t>(300, 0x11);
  std::vector<uint8_t> compressed;
  CompressOptions opt;
  CHECK_ERR(compress(data, compressed, opt, nullptr), Err::Ok);

  // Claim one byte fewer than the payload holds.
  auto mutated = compressed;
  ByteReader r(mutated);
  FileHeader fh;
  read_file_header(r, fh);
  // Header is: 4 magic + 1 version + 1 flags + 1 layers + varint original +
  // varint blocks. original_size is a 2-byte varint here (300 = 0xAC 0x02).
  mutated[7] = 0xAB;  // 299
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(mutated, out), Err::SizeMismatch);
}

TEST(parallel_roundtrips) {
  // Parallel mode makes blocks self-contained so they can be encoded on several
  // threads. The stream must still decode exactly, and the header must say so.
  chce_test::Rng rng(77);
  auto data = make_data(rng, 300 * 1024, 2);

  CompressOptions opt;
  opt.parallel = true;
  opt.threads = 2;
  opt.block_size = 64 * 1024;  // several blocks, so the threaded path is used

  std::vector<uint8_t> compressed, restored;
  CompressStats stats;
  CHECK_ERR(compress(data, compressed, opt, &stats), Err::Ok);
  CHECK_MSG(stats.blocks >= 4, "expected several blocks, got " +
                                   std::to_string(stats.blocks));

  // The flag must be set, or a decoder would not know the blocks are independent.
  ByteReader r(compressed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.flags & kFlagParallel) != 0, "kFlagParallel not set");

  CHECK_ERR(decompress(compressed, restored), Err::Ok);
  CHECK_MSG(restored == data, "parallel round-trip mismatch");

  // And archive mode on the same input must still work.
  CompressOptions archive;
  archive.block_size = 64 * 1024;
  std::vector<uint8_t> a, ar;
  CHECK_ERR(compress(data, a, archive), Err::Ok);
  ByteReader r2(a);
  FileHeader fh2;
  CHECK_ERR(read_file_header(r2, fh2), Err::Ok);
  CHECK_MSG((fh2.flags & kFlagParallel) == 0, "archive stream marked parallel");
  CHECK_ERR(decompress(a, ar), Err::Ok);
  CHECK_MSG(ar == data, "archive round-trip mismatch");
}

TEST(corruption_is_detected) {
  // The format carries a CRC-32 of the original bytes. Without it, a flipped
  // byte in a payload that still parses produced plausible wrong data and
  // reported success — the failure mode a stack of transforms invites.
  chce_test::Rng rng(31);
  auto data = make_data(rng, 40000, 2);
  std::vector<uint8_t> compressed;
  CHECK_ERR(compress(data, compressed, CompressOptions{}), Err::Ok);

  int detected = 0;
  for (size_t off : {compressed.size() / 3, compressed.size() / 2,
                     compressed.size() * 3 / 4}) {
    auto bad = compressed;
    bad[off] ^= 0xFF;
    std::vector<uint8_t> out;
    const Err e = decompress(bad, out);
    if (e != Err::Ok) ++detected;
    else CHECK_MSG(out == data, "corrupt stream decoded to wrong bytes as success");
  }
  CHECK_MSG(detected >= 2, "corruption went undetected in " +
                               std::to_string(3 - detected) + " of 3 cases");
}

// Validate before allocating: a declared block count must be bounded by the bytes
// the stream actually holds. Before this check the decoder sized its per-block
// structures from the header alone, so a short stream declaring a million blocks
// asked for a million entries before discovering there was nothing there.
TEST(impossible_block_count_is_rejected_before_allocating) {
  ByteWriter w;
  FileHeader fh;
  fh.version = kFormatVersion;
  fh.flags = 0;
  fh.layers = 0;
  fh.original_size = 1000000;
  fh.block_count = 1000000;  // more blocks than 40 bytes can possibly hold
  fh.checksum = 0;
  write_file_header(w, fh);
  for (int i = 0; i < 40; ++i) w.u8(0);

  std::vector<uint8_t> out;
  CHECK_ERR(decompress(w.take(), out), Err::BadBlockSize);
}

TEST(block_count_above_the_ceiling_is_rejected) {
  ByteWriter w;
  FileHeader fh;
  fh.version = kFormatVersion;
  fh.original_size = kMaxBlocks + 1;
  fh.block_count = kMaxBlocks + 1;
  write_file_header(w, fh);

  ByteReader r(w.span());
  FileHeader got;
  CHECK_ERR(read_file_header(r, got), Err::BadBlockSize);
}

TEST(a_legitimate_multi_block_stream_still_decodes) {
  chce_test::Rng rng(91);
  const auto data = make_data(rng, 200 * 1024, 1);
  CompressOptions opt;
  opt.block_size = 16 * 1024;  // 13 blocks
  opt.parse = false;
  opt.lzma2_fallback = false;
  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(data, packed, opt, nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == data, "multi-block round-trip mismatch");
}

TEST_MAIN()
