// test_integrity.cpp — per-block SHA-256 and the stream trailer.
//
// Before v4 a stream carried one CRC-32 over the whole original. That answers
// "is this archive intact?" only after every block has been decoded and every
// Layer 0 inverse has run, and it never says which block was damaged. These
// tests pin the new behaviour: the digest algorithm itself, a block that fails
// its own check, and a trailer that disagrees with the block set.
#include <string>
#include <vector>

#include "harness.hpp"

#include "chimera/block.hpp"
#include "chimera/sha256.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"

using namespace chce;

namespace {

std::string hex(const Sha256Digest& d) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  for (uint8_t b : d) {
    s.push_back(kHex[b >> 4]);
    s.push_back(kHex[b & 15]);
  }
  return s;
}

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

// Options that force the native encoder to emit RAW blocks: the body is then the
// plaintext, so a flipped byte reaches the hash check rather than being rejected
// by a backend first.
CompressOptions raw_options(size_t block_size = 1024) {
  CompressOptions opt;
  opt.block_size = block_size;
  opt.parse = false;
  opt.lzma2_fallback = false;
  opt.max_ratio = false;
  opt.bwt = false;
  opt.huffman = false;
  opt.range = false;
  opt.adaptive = false;
  opt.precomp = false;
  opt.jpeg = false;
  opt.gif = false;
  opt.numeric = false;
  opt.dedup = false;
  opt.bcj = false;
  return opt;
}

std::vector<uint8_t> make_data(size_t n) {
  std::vector<uint8_t> d(n);
  uint32_t x = 0x12345678u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    d[i] = static_cast<uint8_t>(x >> 24);
  }
  return d;
}

}  // namespace

// ------------------------------------------------------------------ SHA-256
// FIPS 180-4 vectors. If these pass, the digest is the standard's, not ours.
TEST(sha256_matches_the_standard_vectors) {
  CHECK_MSG(hex(sha256({})) ==
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "empty string vector: " + hex(sha256({})));
  CHECK_MSG(hex(sha256(bytes("abc"))) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "abc vector: " + hex(sha256(bytes("abc"))));
  CHECK_MSG(
      hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
      "two-block vector: " +
          hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))));

  // A 1000-byte input, which crosses the padding boundary differently again.
  const std::vector<uint8_t> kb(1000, 'a');
  CHECK_MSG(hex(sha256(kb)) ==
                "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
            "1000 x 'a' vector: " + hex(sha256(kb)));
}

TEST(sha256_is_sensitive_to_a_single_bit) {
  auto a = bytes("the quick brown fox");
  auto b = a;
  b[3] ^= 1;
  CHECK_MSG(!sha256_equal(sha256(a), sha256(b)), "a one-bit change did not change the digest");
}

// ------------------------------------------------------------- block digests
TEST(round_trip_still_exact) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(data, packed, raw_options(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == data, "round-trip mismatch");
}

TEST(every_block_carries_a_digest_of_its_own_bytes) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, raw_options(1024), nullptr), Err::Ok);

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_EQ(fh.block_count, uint64_t{5});

  size_t checked = 0;
  for (uint64_t i = 0; i < fh.block_count; ++i) {
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::Ok);
    std::span<const uint8_t> body;
    CHECK_MSG(r.take(static_cast<size_t>(bh.compressed_size), body), "body missing");
    // RAW blocks: the body is the plaintext, so the digest must match it.
    CHECK_MSG(bh.mode == Mode::Raw, "expected a RAW block");
    CHECK_MSG(sha256_equal(sha256(body), bh.sha256),
              "block " + std::to_string(i) + " digest does not cover its bytes");
    ++checked;
  }
  CHECK_EQ(checked, size_t{5});
}

// A damaged block must be reported as such — not as a whole-archive CRC failure
// after everything has been decoded.
TEST(a_damaged_block_fails_its_own_check) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, raw_options(1024), nullptr), Err::Ok);

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  BlockHeader bh;
  CHECK_ERR(read_block_header(r, bh), Err::Ok);
  const size_t body_at = r.offset();

  auto damaged = packed;
  damaged[body_at + 7] ^= 0x40;

  std::vector<uint8_t> out;
  CHECK_ERR(decompress(damaged, out), Err::BlockChecksumMismatch);
}

// The trailer binds the ordered set of headers. Editing the stored manifest, or
// the block count it carries, must be caught before the payload is trusted.
TEST(a_damaged_trailer_is_caught) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, raw_options(1024), nullptr), Err::Ok);

  // The trailer is the last 37 bytes: "ZTIX" | count varint (1 byte) | 32-byte
  // digest. Flip a digest byte.
  auto bad_digest = packed;
  bad_digest.back() ^= 0x01;
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(bad_digest, out), Err::ManifestMismatch);

  // Now the count: 33 bytes from the end.
  auto bad_count = packed;
  bad_count[bad_count.size() - 33] = 2;
  CHECK_ERR(decompress(bad_count, out), Err::ManifestMismatch);
}

TEST(a_stream_without_its_trailer_is_rejected) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, raw_options(1024), nullptr), Err::Ok);

  std::vector<uint8_t> cut(packed.begin(), packed.end() - 8);
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(cut, out), Err::Truncated);
}

TEST(bytes_appended_after_the_trailer_are_rejected) {
  const auto data = make_data(5000);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, raw_options(1024), nullptr), Err::Ok);

  auto padded = packed;
  padded.push_back(0);
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(padded, out), Err::TrailingData);
}

// Both decode paths must check the same thing: the parallel path assembles
// blocks from threads and the sequential path appends as it goes.
TEST(the_parallel_path_checks_block_digests_too) {
  const auto data = make_data(300 * 1024);
  CompressOptions opt = raw_options(32 * 1024);
  opt.parallel = true;
  opt.threads = 2;

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(data, packed, opt, nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == data, "parallel round-trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.flags & kFlagParallel) != 0, "parallel flag not set");

  BlockHeader bh;
  CHECK_ERR(read_block_header(r, bh), Err::Ok);
  const size_t body_at = r.offset();
  auto damaged = packed;
  damaged[body_at + 3] ^= 0xff;
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(damaged, out), Err::BlockChecksumMismatch);
}

// The whole-stream backends write their own single block; they must seal it the
// same way or every --max-ratio and Tier-1 stream would be unreadable.
TEST(single_block_paths_are_sealed) {
  const auto data = make_data(20000);

  CompressOptions lz;
  lz.block_size = 64 * 1024;
  lz.parse = false;
  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(data, packed, lz, nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == data, "LZMA2 stream round-trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_EQ(fh.block_count, uint64_t{1});
  BlockHeader bh;
  CHECK_ERR(read_block_header(r, bh), Err::Ok);
  CHECK_MSG(!sha256_equal(bh.sha256, Sha256Digest{}), "single block carries no digest");
}

TEST(an_empty_stream_round_trips_with_a_trailer) {
  const std::vector<uint8_t> empty;
  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(empty, packed, raw_options(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back.empty(), "empty input did not round-trip");
}

TEST_MAIN()
