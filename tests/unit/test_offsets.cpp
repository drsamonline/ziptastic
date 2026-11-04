// test_offsets.cpp — the v8 block offset table.
//
// The table is only worth its eight bytes a block if it is *true*, so these tests are
// mostly about the ways it can lie: an offset that points at nothing, a table that
// disagrees with the headers, a tail that was truncated, a block whose payload was
// damaged. The walking decoder checks the table it can see; `read_index` checks the one
// it cannot, without decoding anything.
#include <algorithm>
#include <string>
#include <vector>

#include "harness.hpp"

#include "chimera/block.hpp"
#include "chimera/block_index.hpp"
#include "chimera/format.hpp"
#include "chimera/sha256.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"

using namespace chce;

namespace {

// Bytes a LEB128 varint occupies. The trailer's count is one, and the table starts after
// it, so a test that wants to edit the table has to know where that is.
size_t varint_size(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

std::vector<uint8_t> make_data(size_t n, uint32_t seed = 0x12345678u) {
  std::vector<uint8_t> d(n);
  uint32_t x = seed;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    d[i] = static_cast<uint8_t>(x >> 24);
  }
  return d;
}

// RAW blocks in parallel mode: the body is the plaintext, so a flipped byte reaches the
// digest rather than being rejected by a backend first, and every block is independent.
CompressOptions parallel_raw(size_t block_size) {
  CompressOptions opt;
  opt.block_size = block_size;
  opt.parallel = true;
  opt.threads = 2;
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
  opt.base64 = false;
  opt.wav = false;
  opt.sqlite = false;
  opt.ecc = false;
  opt.bmp = false;
  opt.dedup = false;
  opt.bcj = false;
  return opt;
}

// Where the trailer begins in a stream, computed the way a reader computes it.
uint64_t trailer_at(const std::vector<uint8_t>& packed, uint64_t block_count) {
  return packed.size() - trailer_bytes(block_count);
}

// The offset of entry `i` of the table.
uint64_t table_entry_at(const std::vector<uint8_t>& packed, uint64_t block_count,
                        uint64_t i) {
  ByteReader r(packed);
  FileHeader fh;
  (void)read_file_header(r, fh);
  return trailer_at(packed, block_count) + 4 + varint_size(fh.block_count) + 32 + 8 * i;
}

}  // namespace

// The trailer's length is arithmetic a reader relies on to find it from the end. If the
// writer and `trailer_bytes` ever disagree, every v8 stream becomes unreadable at once.
TEST(the_trailer_is_the_size_the_reader_computes) {
  for (uint64_t count : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{128},
                         uint64_t{300}, uint64_t{70000}}) {
    std::vector<uint64_t> offsets(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) offsets[static_cast<size_t>(i)] = 20 + i * 40;

    ByteWriter w;
    w.bytes("pad", 3);  // something in front, so an absolute offset means something
    write_trailer(w, count, Sha256Digest{}, offsets);

    const size_t expected = 3 + trailer_bytes(count);
    CHECK_EQ(w.size(), expected);

    // And it parses back to the same table.
    ByteReader r(std::span<const uint8_t>(w.buffer()).subspan(3));
    TrailerInfo info;
    CHECK_ERR(read_trailer(r, info), Err::Ok);
    CHECK(r.at_end());
    CHECK_EQ(info.block_count, count);
    CHECK_EQ(info.offsets.size(), static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
      CHECK_EQ(info.offsets[static_cast<size_t>(i)], offsets[static_cast<size_t>(i)]);
    }
  }
}

// Every recorded offset lands on a block header, the blocks tile the stream with no gap,
// and the last one ends where the trailer begins. That is the whole structural claim.
TEST(every_block_offset_points_at_its_block) {
  const auto data = make_data(40 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.size(), size_t{10});
  CHECK_EQ(idx.header.block_count, uint64_t{10});
  CHECK_EQ(idx.stream_offset, uint64_t{0});

  // The first block starts where the file header ends.
  ByteReader hr(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(hr, fh), Err::Ok);
  CHECK_EQ(idx.block_offsets[0], static_cast<uint64_t>(hr.offset()));

  uint64_t previous_end = hr.offset();
  for (size_t i = 0; i < idx.size(); ++i) {
    CHECK_EQ(idx.block_offsets[i], previous_end);

    // The header really is there, and it is the one the index reports.
    ByteReader br(std::span<const uint8_t>(packed).subspan(idx.block_offsets[i]));
    BlockHeader bh;
    CHECK_ERR(read_block_header(br, bh), Err::Ok);
    CHECK_EQ(mode_byte(bh.mode), mode_byte(idx.blocks[i].mode));
    CHECK_EQ(bh.compressed_size, idx.blocks[i].compressed_size);
    CHECK_EQ(bh.uncompressed_size, idx.blocks[i].uncompressed_size);
    CHECK_EQ(idx.body_offsets[i], idx.block_offsets[i] + br.offset());
    previous_end = idx.body_offsets[i] + bh.compressed_size;
  }
  CHECK_EQ(previous_end, idx.trailer_offset);
  CHECK_EQ(idx.trailer_offset, trailer_at(packed, fh.block_count));
}

// The index must say what a walk from the front says, or one of the two is wrong.
TEST(the_index_agrees_with_a_front_walk) {
  const auto data = make_data(30 * 1024, 0xABCDEF01u);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);

  std::vector<uint64_t> walked;
  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  for (uint64_t i = 0; i < fh.block_count; ++i) {
    walked.push_back(r.offset());
    BlockHeader bh;
    CHECK_ERR(read_block_header(r, bh), Err::Ok);
    std::span<const uint8_t> body;
    CHECK(r.take(static_cast<size_t>(bh.compressed_size), body));
  }
  CHECK_EQ(walked.size(), idx.block_offsets.size());
  for (size_t i = 0; i < walked.size() && i < idx.block_offsets.size(); ++i) {
    CHECK_EQ(idx.block_offsets[i], walked[i]);
  }
}

// Reading the index must not read the payload. On a 3 MiB stream of incompressible bytes
// the index is a few hundred bytes — which is the entire point of the table.
TEST(the_index_reads_no_payload) {
  const auto data = make_data(3 * 1024 * 1024, 0x5EED1234u);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(1024 * 1024), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.size(), size_t{3});
  CHECK_MSG(idx.bytes_read * 1000 < packed.size(),
            "index read " + std::to_string(idx.bytes_read) + " of " +
                std::to_string(packed.size()) + " bytes");
}

TEST(a_single_block_stream_has_a_one_entry_table) {
  const auto data = make_data(20000);
  std::vector<uint8_t> packed;
  CompressOptions opt;  // default: the LZMA2 fallback usually wins here
  CHECK_ERR(compress(data, packed, opt, nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.header.block_count, uint64_t{1});
  CHECK_EQ(idx.block_offsets.size(), size_t{1});
  CHECK_EQ(idx.uncompressed_offset(0), uint64_t{0});
}

TEST(an_empty_stream_has_an_empty_table) {
  const std::vector<uint8_t> empty;
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(empty, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.header.block_count, uint64_t{0});
  CHECK(idx.block_offsets.empty());
  CHECK(idx.blocks.empty());
  CHECK_EQ(idx.trailer_offset, static_cast<uint64_t>(packed.size() - trailer_bytes(0)));
}

// One block, decoded from its offset, is the bytes that block contributed.
TEST(decode_blocks_matches_the_full_decode) {
  const auto data = make_data(24 * 1024, 0x0BADF00Du);
  std::vector<uint8_t> packed, whole;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, whole), Err::Ok);
  CHECK_MSG(whole == data, "full decode mismatch");

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.size(), size_t{6});

  for (size_t i = 0; i < idx.size(); ++i) {
    std::vector<uint8_t> part;
    CHECK_ERR(decode_blocks(packed, idx, i, i, part), Err::Ok);
    const uint64_t at = idx.uncompressed_offset(i);
    CHECK_EQ(part.size(), static_cast<size_t>(idx.blocks[i].uncompressed_size));
    CHECK_MSG(std::equal(part.begin(), part.end(), whole.begin() + static_cast<long>(at)),
              "block " + std::to_string(i) + " is not the slice it claims");
  }

  // A range, and the range that covers everything (which checks the whole-stream CRC).
  std::vector<uint8_t> tail;
  CHECK_ERR(decode_blocks(packed, idx, 2, 4, tail), Err::Ok);
  const uint64_t from = idx.uncompressed_offset(2);
  CHECK(std::equal(tail.begin(), tail.end(), whole.begin() + static_cast<long>(from)));

  std::vector<uint8_t> all;
  CHECK_ERR(decode_blocks(packed, idx, 0, idx.size() - 1, all), Err::Ok);
  CHECK_MSG(all == data, "decode_blocks over every block is not the original");
}

// The threaded entry point must produce the same bytes as the single-threaded one, at
// every thread count including the ones that do not divide the block count evenly.
TEST(decode_blocks_parallel_matches_the_walking_decode) {
  const auto data = make_data(24 * 1024, 0x0DDBA11u);
  std::vector<uint8_t> packed, whole;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, whole), Err::Ok);
  CHECK_MSG(whole == data, "full decode mismatch");

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.size(), size_t{6});

  // 1, 2, 3 and 8: 8 is more threads than blocks, which must not lose one.
  for (unsigned threads : {1u, 2u, 3u, 8u}) {
    std::vector<uint8_t> out;
    CHECK_ERR(decode_blocks_parallel(packed, idx, threads, out), Err::Ok);
    CHECK_MSG(out == data,
              "parallel decode with " + std::to_string(threads) + " threads differs");
  }

  // A damaged block is still named, whichever thread found it.
  auto damaged = packed;
  damaged[static_cast<size_t>(idx.body_offsets[4] + 3)] ^= 0x80;
  std::vector<uint8_t> out;
  uint64_t bad = kNoBlock;
  CHECK_ERR(decode_blocks_parallel(damaged, idx, 2, out, &bad),
            Err::BlockChecksumMismatch);
  CHECK_EQ(bad, uint64_t{4});

  // And the refusals hold on this entry point too.
  idx.header.layers = kLayerNumeric;
  CHECK_ERR(decode_blocks_parallel(packed, idx, 2, out), Err::NotSelfContained);
}

// Archive mode is the mode where a block may reference an earlier one. Asking for one
// block on its own has no answer, and the answer must say so rather than guess.
TEST(decode_blocks_refuses_archive_mode) {
  const auto data = make_data(24 * 1024);
  CompressOptions opt = parallel_raw(4096);
  opt.parallel = false;  // archive mode

  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, opt, nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_EQ(idx.size(), size_t{6});

  std::vector<uint8_t> out;
  CHECK_ERR(decode_blocks(packed, idx, 1, 1, out), Err::NotSelfContained);
  CHECK(out.empty());
}

// A stream whose payload carries a Layer 0 transform is addressable but not sliceable:
// the inverse chain needs the whole stream, so a block's plaintext is an intermediate.
TEST(decode_blocks_refuses_a_transformed_stream) {
  const auto data = make_data(8 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  // The encoder ran no transform on this data; claim it did and the request must be
  // refused rather than answered with an intermediate.
  idx.header.layers = kLayerNumeric;
  std::vector<uint8_t> out;
  CHECK_ERR(decode_blocks(packed, idx, 0, 0, out), Err::NotSelfContained);
  idx.header.layers = 0;
  idx.header.flags = static_cast<uint8_t>(idx.header.flags | kFlagPrecomp);
  CHECK_ERR(decode_blocks(packed, idx, 0, 0, out), Err::NotSelfContained);
}

// A damaged block is named by its index and contributes nothing — the same promise the
// walking decoder makes.
TEST(decode_blocks_names_a_damaged_block) {
  const auto data = make_data(20 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);

  auto damaged = packed;
  damaged[static_cast<size_t>(idx.body_offsets[2] + 5)] ^= 0x40;

  std::vector<uint8_t> out;
  uint64_t bad = kNoBlock;
  CHECK_ERR(decode_blocks(damaged, idx, 2, 2, out, &bad), Err::BlockChecksumMismatch);
  CHECK_EQ(bad, uint64_t{2});
  CHECK(out.empty());

  // Block 1 is untouched, so it still decodes.
  std::vector<uint8_t> good;
  CHECK_ERR(decode_blocks(damaged, idx, 1, 1, good), Err::Ok);
  CHECK_EQ(good.size(), size_t{4096});
}

// The table is data, and data can be wrong. A table that does not describe the blocks
// must be refused: it is the difference between an index and a guess.
TEST(a_corrupted_offset_table_is_refused) {
  const auto data = make_data(20 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  const uint64_t count = idx.header.block_count;

  // Point entry 3 at entry 2's position: the table is then not strictly increasing.
  auto duplicate = packed;
  const uint64_t entry2 = table_entry_at(packed, count, 2);
  for (int b = 0; b < 8; ++b) {
    duplicate[static_cast<size_t>(table_entry_at(packed, count, 3) + b)] = duplicate[static_cast<size_t>(entry2 + b)];
  }
  StreamIndex ignored;
  CHECK_ERR(read_index(duplicate, ignored), Err::Corrupt);

  // Move one entry eight bytes into the payload it points at: still increasing, still a
  // well-formed table, and it lands on a block header that is not the one the manifest
  // covers — or on nothing parseable at all. Either way it must not be accepted.
  auto shifted = packed;
  shifted[static_cast<size_t>(table_entry_at(packed, count, 1))] += 8;
  StreamIndex ignored2;
  const Err e = read_index(shifted, ignored2);
  CHECK_MSG(e != Err::Ok, "a shifted offset was accepted");
}

// Truncation inside the table, and truncation of the marker that locates it.
TEST(a_truncated_tail_is_refused) {
  const auto data = make_data(20 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  for (size_t cut : {size_t{1}, size_t{4}, size_t{8}, size_t{40}}) {
    std::vector<uint8_t> shorter(packed.begin(), packed.end() - static_cast<long>(cut));
    StreamIndex idx;
    const Err e = read_index(shorter, idx);
    CHECK_MSG(e != Err::Ok, "a stream cut by " + std::to_string(cut) + " bytes was accepted");
  }

  // The full stream, with its last byte intact, must still read.
  StreamIndex ok;
  CHECK_ERR(read_index(packed, ok), Err::Ok);
}

// A self-extracting stub puts bytes in front of the magic. Offsets are stream-relative,
// so the index moves with the stream and nothing in the table needs rewriting.
TEST(the_table_survives_a_prefix) {
  const auto data = make_data(20 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex plain;
  CHECK_ERR(read_index(packed, plain), Err::Ok);

  std::vector<uint8_t> prefixed(100, 0x7F);
  prefixed.insert(prefixed.end(), packed.begin(), packed.end());

  StreamIndex idx;
  CHECK_ERR(read_index(prefixed, idx), Err::Ok);
  CHECK_EQ(idx.stream_offset, uint64_t{100});
  CHECK_EQ(idx.size(), plain.size());
  for (size_t i = 0; i < idx.size(); ++i) {
    CHECK_EQ(idx.block_offsets[i], plain.block_offsets[i] + 100);
  }

  std::vector<uint8_t> out;
  CHECK_ERR(decode_blocks(prefixed, idx, 0, idx.size() - 1, out), Err::Ok);
  CHECK_MSG(out == data, "prefixed stream did not decode");
}

// The walking decoder knows where every block was, so it can check the table against
// reality. A table edited to agree with itself but not with the stream must fail there
// too — otherwise the two readers disagree about the same file.
TEST(the_walking_decoder_checks_the_table) {
  const auto data = make_data(20 * 1024);
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(data, packed, parallel_raw(4096), nullptr), Err::Ok);

  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);

  // +1 on an offset keeps the table increasing, and the walk still decodes every block,
  // so the only thing that can catch this is the comparison against what the walk saw.
  auto edited = packed;
  edited[static_cast<size_t>(table_entry_at(packed, idx.header.block_count, 0))] += 1;
  std::vector<uint8_t> out;
  CHECK_ERR(decompress(edited, out), Err::ManifestMismatch);
}

TEST_MAIN()
