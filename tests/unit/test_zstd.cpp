// test_zstd.cpp — the zstd transform.
//
// zstd has a magic number, so unlike brotli there is something to recognise. What still has to be
// recovered by trial is the level *and* the framing: a stream written to a pipe omits its content
// size and its block boundaries follow the writer's chunking, so a seekable encode at the same level
// does not reproduce it. Both shapes are tested here because both appear in the wild.
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "precompressors/zstd_wrap.hpp"

#include <zstd.h>

using namespace chce;

namespace {

std::vector<uint8_t> make_zst(const std::vector<uint8_t>& raw, int level, bool streaming) {
  std::vector<uint8_t> out;
  ZSTD_CCtx* cs = ZSTD_createCCtx();
  if (cs == nullptr) return out;
  if (streaming) ZSTD_CCtx_setParameter(cs, ZSTD_c_contentSizeFlag, 0);
  ZSTD_CCtx_setParameter(cs, ZSTD_c_compressionLevel, level);
  out.resize(ZSTD_compressBound(raw.size()));
  size_t n = 0;
  if (streaming) {
    ZSTD_outBuffer ob{out.data(), out.size(), 0};
    size_t r = 0;
    constexpr size_t kChunk = 128 * 1024;
    for (size_t off = 0; off < raw.size(); off += kChunk) {
      ZSTD_inBuffer ib{raw.data() + off, std::min(kChunk, raw.size() - off), 0};
      r = ZSTD_compressStream2(cs, &ob, &ib, ZSTD_e_continue);
      if (ZSTD_isError(r)) break;
    }
    if (!ZSTD_isError(r)) {
      ZSTD_inBuffer empty{raw.data(), 0, 0};
      r = ZSTD_compressStream2(cs, &ob, &empty, ZSTD_e_end);
    }
    n = ZSTD_isError(r) ? 0 : ob.pos;
  } else {
    n = ZSTD_compress2(cs, out.data(), out.size(), raw.data(), raw.size());
    if (ZSTD_isError(n)) n = 0;
  }
  ZSTD_freeCCtx(cs);
  out.resize(n);
  return out;
}

std::vector<uint8_t> prose(size_t n) {
  static const char* kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
                                 "and", "then", "walks", "back", "again", "through", "the", "field"};
  std::vector<uint8_t> out;
  out.reserve(n);
  uint32_t x = 31;
  while (out.size() < n) {
    x = x * 1664525u + 1013904223u;
    const char* w = kWords[(x >> 16) % 16];
    out.insert(out.end(), w, w + std::strlen(w));
    out.push_back((x >> 8) & 1 ? ' ' : '\n');
  }
  out.resize(n);
  return out;
}

CompressOptions precomp_on() {
  CompressOptions opt;
  opt.precomp = true;
  opt.block_size = 64 * 1024;
  return opt;
}

}  // namespace

TEST(a_zstd_stream_is_recognised) {
  const auto raw = prose(8192);
  const auto z = make_zst(raw, 3, false);
  CHECK(!z.empty());
  CHECK(zstd_likely(z));
  CHECK(!zstd_likely(std::vector<uint8_t>{0x28, 0xB5, 0x2F, 0x00}));
  CHECK(!zstd_likely(std::vector<uint8_t>{}));
}

// Both framings, at a level inside the shortlist and one at its edge.
TEST(the_payload_rebuilds_the_stream_exactly) {
  const auto raw = prose(200000);
  for (int level : {3, 19}) {
    for (bool streaming : {false, true}) {
      const auto z = make_zst(raw, level, streaming);
      CHECK(!z.empty());
      std::vector<uint8_t> payload;
      const std::string what =
          "level " + std::to_string(level) + (streaming ? " streaming" : " seekable");
      CHECK_MSG(zstd_unpack(z, payload), what + " was not unpacked");
      std::vector<uint8_t> again;
      CHECK(zstd_repack(payload, again));
      CHECK_MSG(again == z, what + " did not rebuild byte-exactly");
    }
  }
}

TEST(anything_that_is_not_a_zstd_stream_is_refused) {
  std::vector<uint8_t> not_zstd(20000);
  uint32_t x = 5;
  for (auto& b : not_zstd) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(x >> 24);
  }
  std::vector<uint8_t> payload;
  CHECK(!zstd_unpack(not_zstd, payload));
  CHECK(payload.empty());

  // Right magic, damaged body: must decline rather than guess.
  auto z = make_zst(prose(30000), 3, false);
  CHECK(!z.empty());
  z[z.size() / 2] ^= 0xFF;
  CHECK_MSG(!zstd_unpack(z, payload), "a damaged stream was accepted");

  std::vector<uint8_t> bad = {'Z', 'S', '0', '1', 99, 0, 0};
  std::vector<uint8_t> out;
  CHECK(!zstd_repack(bad, out));
}

TEST(a_tar_zst_compresses_and_round_trips) {
  const auto raw = prose(150000);
  const auto z = make_zst(raw, 19, false);
  CHECK(!z.empty());

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(z, packed, precomp_on(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == z, "round trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.layers3 & kLayer3Zstd) != 0, "the zstd layer bit was not set");

  CompressOptions plain = precomp_on();
  plain.precomp = false;
  std::vector<uint8_t> stored;
  CHECK_ERR(compress(z, stored, plain, nullptr), Err::Ok);
  CHECK_MSG(packed.size() < stored.size(),
            "the transform did not help: " + std::to_string(packed.size()) + " against " +
                std::to_string(stored.size()));
}

TEST_MAIN()
