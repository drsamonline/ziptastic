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

void check_roundtrip(const std::vector<uint8_t>& data, size_t block_size) {
  CompressOptions opt;
  opt.block_size = block_size;

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
  CHECK_EQ(fh.block_count, expect_blocks);
  CHECK_EQ(stats.blocks, expect_blocks);

  std::vector<uint8_t> restored;
  DecompressReport rep;
  CHECK_ERR(decompress(compressed, restored, &rep), Err::Ok);
  CHECK_EQ(restored.size(), data.size());
  CHECK_EQ(rep.blocks, expect_blocks);
  CHECK_MSG(restored == data, "round-trip mismatch at " + std::to_string(data.size()) +
                                  " bytes, block " + std::to_string(block_size));

  // RAW mode must be self-describing: header + block headers + payload.
  CHECK(stats.raw_blocks == stats.blocks);

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
    check_roundtrip(make_data(rng, n, static_cast<int>(n % 4)), kDefaultBlockSize);
  }
}

TEST(block_boundaries) {
  chce_test::Rng rng(7);
  const size_t bs = 1024;  // small block size keeps the test fast
  const size_t sizes[] = {bs - 1, bs, bs + 1, 2 * bs - 1, 2 * bs, 2 * bs + 1, 10 * bs};
  for (size_t n : sizes) {
    check_roundtrip(make_data(rng, n, 0), bs);
    check_roundtrip(make_data(rng, n, 2), bs);
  }
}

TEST(one_hundred_random_files) {
  chce_test::Rng rng(0xC0FFEE);
  for (int i = 0; i < 100; ++i) {
    // 0 B .. 1 MB, biased toward the small end so the sweep stays quick.
    const size_t n = static_cast<size_t>(rng.below(1024 * 1024));
    check_roundtrip(make_data(rng, n, i % 4), kDefaultBlockSize);
  }
}

TEST(varying_block_sizes) {
  chce_test::Rng rng(99);
  const auto data = make_data(rng, 300 * 1024, 2);
  for (size_t bs : {size_t{1} * 1024, size_t{7} * 1024, size_t{64} * 1024,
                    size_t{300} * 1024, size_t{1024} * 1024}) {
    check_roundtrip(data, bs);
  }
}

TEST(all_modes_rejected_but_raw) {
  // Every mode byte is defined, but Phase 0 implements RAW only: the decoder
  // must reject the others cleanly rather than mis-parse them.
  const auto data = std::vector<uint8_t>(100, 0x5A);
  std::vector<uint8_t> compressed;
  CompressOptions opt;
  CHECK_ERR(compress(data, compressed, opt, nullptr), Err::Ok);

  for (uint8_t m = 1; m <= mode_byte(Mode::Ppmd); ++m) {
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
