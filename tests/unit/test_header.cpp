// test_header.cpp — header serialisation and rejection of malformed headers.
//
// This is the Phase 0 "header fuzz" requirement: mutated magic, version,
// flags and lengths must produce a specific error, never a crash.
#include <algorithm>
#include <cstring>

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> make_stream(uint64_t original_size, uint64_t block_count) {
  ByteWriter w;
  FileHeader h;
  h.original_size = original_size;
  h.block_count = block_count;
  write_file_header(w, h);
  return w.take();
}

std::vector<uint8_t> valid_file_header() {
  ByteWriter w;
  FileHeader h;
  h.flags = kFlagCoarse;
  h.original_size = 1000;
  h.block_count = 4;
  write_file_header(w, h);
  return w.take();
}

}  // namespace

TEST(file_header_roundtrip) {
  const uint64_t sizes[] = {0, 1, 255, 256, 65535, 1ull << 32, (1ull << 40) - 1};
  for (uint64_t sz : sizes) {
    // A block carries at least one byte, so the count can never exceed sz.
    const uint64_t blocks = sz == 0 ? 0 : std::min<uint64_t>(1 + (sz % 7), sz);
    ByteWriter w;
    FileHeader h;
    h.flags = static_cast<uint8_t>(kFlagCoarse | kFlagPrecomp);
    h.original_size = sz;
    h.block_count = blocks;
    write_file_header(w, h);

    ByteReader r(w.span());
    FileHeader got;
    CHECK_ERR(read_file_header(r, got), Err::Ok);
    CHECK_EQ(got.version, uint8_t{kFormatVersion});
    CHECK_EQ(got.flags, h.flags);
    CHECK_EQ(got.original_size, sz);
    CHECK_EQ(got.block_count, blocks);
    CHECK(r.at_end());
  }
}

TEST(file_header_bad_magic) {
  for (size_t i = 0; i < 4; ++i) {
    auto buf = valid_file_header();
    buf[i] ^= 0x01;
    ByteReader r(buf);
    FileHeader h;
    CHECK_ERR(read_file_header(r, h), Err::BadMagic);
  }
  // Wrong magic entirely.
  const std::vector<uint8_t> zip = {'P', 'K', 3, 4, 1, 0, 0, 0};
  ByteReader r(zip);
  FileHeader h;
  CHECK_ERR(read_file_header(r, h), Err::BadMagic);
}

TEST(file_header_bad_version) {
  auto buf = valid_file_header();
  buf[4] = 2;  // version 2 does not exist yet
  ByteReader r(buf);
  FileHeader h;
  CHECK_ERR(read_file_header(r, h), Err::BadVersion);

  buf[4] = 0;
  ByteReader r0(buf);
  CHECK_ERR(read_file_header(r0, h), Err::BadVersion);
}

TEST(file_header_bad_flags) {
  auto buf = valid_file_header();
  // A bit outside kFlagMask. This has moved twice now as flags were added
  // (0x08 became kFlagBcj, then 0x10 became kFlagParallel), which is the test
  // doing its job: a new flag must be registered in kFlagMask or rejected.
  buf[5] = 0x80;
  ByteReader r(buf);
  FileHeader h;
  CHECK_ERR(read_file_header(r, h), Err::BadFlags);

  buf[5] = 0xFF;
  ByteReader r2(buf);
  CHECK_ERR(read_file_header(r2, h), Err::BadFlags);

  // All defined flags at once is fine.
  buf[5] = kFlagMask;
  ByteReader r3(buf);
  CHECK_ERR(read_file_header(r3, h), Err::Ok);
}

TEST(file_header_truncated) {
  const auto full = valid_file_header();
  for (size_t n = 0; n < full.size(); ++n) {
    ByteReader r(std::span<const uint8_t>(full.data(), n));
    FileHeader h;
    const Err e = read_file_header(r, h);
    CHECK_MSG(e != Err::Ok, "prefix of " + std::to_string(n) + " bytes accepted");
  }
}

TEST(file_header_impossible_counts) {
  // block_count cannot exceed original_size (a block is never empty).
  auto buf = make_stream(10, 11);
  ByteReader r(buf);
  FileHeader h;
  CHECK_ERR(read_file_header(r, h), Err::BadBlockSize);

  // A declared size beyond the 1 TiB ceiling is rejected before allocation.
  ByteWriter w;
  w.bytes(kMagic, sizeof(kMagic));
  w.u8(kFormatVersion);
  w.u8(0);
  w.varint(kMaxStreamSize + 1);
  w.varint(1);
  ByteReader r2(w.span());
  CHECK_ERR(read_file_header(r2, h), Err::BadBlockSize);
}

TEST(block_header_roundtrip) {
  const BlockHeader cases[] = {
      {Mode::Raw, 1, 1},
      {Mode::Raw, 256 * 1024, 256 * 1024},
      {Mode::Lzma2Fallback, 1024, 12},
      {Mode::RansCtx, 4096, 100},
  };
  for (const auto& bh : cases) {
    ByteWriter w;
    write_block_header(w, bh);
    // Payload placeholder so compressed_size fits.
    std::vector<uint8_t> payload(static_cast<size_t>(bh.compressed_size), 0xAA);
    w.bytes(payload);

    ByteReader r(w.span());
    BlockHeader got;
    CHECK_ERR(read_block_header(r, got), Err::Ok);
    CHECK_EQ(mode_byte(got.mode), mode_byte(bh.mode));
    CHECK_EQ(got.uncompressed_size, bh.uncompressed_size);
    CHECK_EQ(got.compressed_size, bh.compressed_size);
  }
}

TEST(block_header_bad_mode) {
  // 0x0B (LZ_RAW) is a real mode; the first undefined byte is 0x0C.
  for (uint8_t mode : {uint8_t{0x0C}, uint8_t{0x7F}, uint8_t{0xFF}}) {
    ByteWriter w;
    w.u8(mode);
    w.varint(10);
    w.varint(10);
    ByteReader r(w.span());
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::BadMode);
  }
}

TEST(block_header_bad_sizes) {
  // Zero-length block is invalid.
  {
    ByteWriter w;
    w.u8(mode_byte(Mode::Raw));
    w.varint(0);
    w.varint(0);
    ByteReader r(w.span());
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::BadBlockSize);
  }
  // Oversized block.
  {
    ByteWriter w;
    w.u8(mode_byte(Mode::Raw));
    w.varint(kMaxBlockSize + 1);
    w.varint(4);
    w.bytes("abcd", 4);
    ByteReader r(w.span());
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::BadBlockSize);
  }
  // compressed_size larger than what remains.
  {
    ByteWriter w;
    w.u8(mode_byte(Mode::Raw));
    w.varint(100);
    w.varint(100);
    w.bytes("abc", 3);
    ByteReader r(w.span());
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::Truncated);
  }
}

TEST(block_header_truncated) {
  ByteWriter w;
  w.u8(mode_byte(Mode::Raw));
  w.varint(300);
  w.varint(300);
  w.bytes(std::vector<uint8_t>(300, 0).data(), 300);
  const auto full = w.buffer();
  for (size_t n = 0; n < 4; ++n) {  // header fields only
    ByteReader r(std::span<const uint8_t>(full.data(), n));
    BlockHeader bh;
    CHECK_MSG(read_block_header(r, bh) != Err::Ok,
              "header prefix of " + std::to_string(n) + " bytes accepted");
  }
}

TEST_MAIN()
