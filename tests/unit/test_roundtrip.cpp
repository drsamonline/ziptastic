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
                     bool use_fallback = true) {
  CompressOptions opt;
  opt.block_size = block_size;
  opt.lzma2_fallback = use_fallback;

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
  CHECK_EQ(stats.raw_blocks + stats.lzma2_blocks, stats.blocks);

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
  chce_test::Rng rng(42);
  const auto data = make_data(rng, 200 * 1024, 2);  // repetitive text

  CompressOptions opt;
  std::vector<uint8_t> native, fallback;
  CompressStats native_stats, fallback_stats;
  opt.lzma2_fallback = false;
  CHECK_ERR(compress(data, native, opt, &native_stats), Err::Ok);
  opt.lzma2_fallback = true;
  CHECK_ERR(compress(data, fallback, opt, &fallback_stats), Err::Ok);

  CHECK(fallback_stats.lzma2_stream);
  CHECK_EQ(fallback_stats.blocks, uint64_t{1});
  CHECK_MSG(fallback.size() < native.size(),
            "fallback " + std::to_string(fallback.size()) +
                " not smaller than native " + std::to_string(native.size()));

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
  CHECK_MSG(out.size() > data.size() && out.size() - data.size() < 64,
            "unexpected framing overhead " +
                std::to_string(out.size() - data.size()));
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
    if (m == mode_byte(Mode::Lzma2Fallback)) continue;  // implemented
    // Rewrite the single block's mode byte.
    ByteReader r(compressed);
    FileHeader fh;
    read_file_header(r, fh);
    const size_t mode_offset = r.offset();
    auto mutated = compressed;
    mutated[mode_offset] = m;
    std::vector<uint8_t> out;
    CHECK_ERR(decompress(mutated, out), Err::BadMode);
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
  // Header is: 4 magic + 1 version + 1 flags + varint original + varint blocks.
  // original_size is a 2-byte varint here (300 = 0xAC 0x02).
  mutated[6] = 0xAB;  // 299
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(mutated, out), Err::SizeMismatch);
}

TEST_MAIN()
