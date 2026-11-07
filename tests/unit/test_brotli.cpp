// test_brotli.cpp — the brotli transform.
//
// Two things make this different from the xz suite next door, and both are what these tests are
// about. Brotli has **no magic number**, so there is nothing to recognise: the only test is that
// the stream decodes and rebuilds byte for byte. And the producing quality is **not in the stream**,
// so the encoder has to guess it from a shortlist and prove the guess.
//
// The fixtures are built here with the vendored brotli rather than checked in, because the point is
// that encoder's own output and a stale binary fixture would stop testing it.
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "precompressors/brotli_wrap.hpp"

#include <brotli/encode.h>

using namespace chce;

namespace {

std::vector<uint8_t> make_br(const std::vector<uint8_t>& raw, uint8_t quality, uint8_t lgwin = 22) {
  std::vector<uint8_t> out;
  BrotliEncoderState* st = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  if (st == nullptr) return out;
  BrotliEncoderSetParameter(st, BROTLI_PARAM_QUALITY, quality);
  BrotliEncoderSetParameter(st, BROTLI_PARAM_LGWIN, lgwin);
  const uint8_t* next_in = raw.data();
  size_t avail_in = raw.size();
  uint8_t buf[1 << 16];
  for (;;) {
    uint8_t* next_out = buf;
    size_t avail_out = sizeof(buf);
    if (BrotliEncoderCompressStream(st, BROTLI_OPERATION_FINISH, &avail_in, &next_in, &avail_out,
                                    &next_out, nullptr) != BROTLI_TRUE) {
      out.clear();
      break;
    }
    out.insert(out.end(), buf, next_out);
    if (BrotliEncoderIsFinished(st) == BROTLI_TRUE) break;
  }
  BrotliEncoderDestroyInstance(st);
  return out;
}

std::vector<uint8_t> prose(size_t n) {
  static const char* kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
                                 "and", "then", "walks", "back", "again", "through", "the", "field"};
  std::vector<uint8_t> out;
  out.reserve(n);
  uint32_t x = 99;
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

// Every quality in the shortlist must be recovered, and the stream rebuilt exactly. A quality
// outside it is expected to be refused — that is the transform declining, not a bug.
TEST(the_payload_rebuilds_the_stream_exactly) {
  const auto raw = prose(60000);
  for (uint8_t q : {uint8_t{1}, uint8_t{5}, uint8_t{9}, uint8_t{11}}) {
    const auto br = make_br(raw, q);
    CHECK(!br.empty());

    std::vector<uint8_t> payload;
    CHECK_MSG(brotli_unpack(br, payload), "quality " + std::to_string(q) + " was not unpacked");

    std::vector<uint8_t> again;
    CHECK(brotli_repack(payload, again));
    CHECK_MSG(again == br, "quality " + std::to_string(q) + " did not rebuild byte-exactly");
  }
}

// No magic number means a wrong guess must be caught by the verifier, not by a header check.
TEST(anything_that_is_not_a_brotli_stream_is_refused) {
  std::vector<uint8_t> randomish(40000);
  uint32_t x = 7;
  for (auto& b : randomish) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(x >> 24);
  }
  std::vector<uint8_t> payload;
  CHECK_MSG(!brotli_unpack(randomish, payload), "random bytes were accepted as brotli");
  CHECK(payload.empty());

  // A stream with something appended is not a bare brotli stream: the tail would be dropped.
  const auto raw = prose(20000);
  auto br = make_br(raw, 11);
  CHECK(!br.empty());
  br.push_back(0x41);
  CHECK_MSG(!brotli_unpack(br, payload), "a stream with a tail was accepted");

  // And a payload whose recipe is nonsense.
  std::vector<uint8_t> bad = {'B', 'R', '0', '1', 99, 0, 22, 0};
  std::vector<uint8_t> out;
  CHECK(!brotli_repack(bad, out));
}

// The whole point: a .tar.br must come out smaller than it went in, with the layer bit set, and
// decode back to the original file.
TEST(a_tar_br_compresses_and_round_trips) {
  const auto raw = prose(150000);
  const auto br = make_br(raw, 11);
  CHECK(!br.empty());

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(br, packed, precomp_on(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == br, "round trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.layers3 & kLayer3Brotli) != 0, "the brotli layer bit was not set");

  CompressOptions plain = precomp_on();
  plain.precomp = false;
  std::vector<uint8_t> stored;
  CHECK_ERR(compress(br, stored, plain, nullptr), Err::Ok);
  CHECK_MSG(packed.size() < stored.size(),
            "the transform did not help: " + std::to_string(packed.size()) + " against " +
                std::to_string(stored.size()));
}

TEST_MAIN()
