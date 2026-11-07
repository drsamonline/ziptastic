// test_xz.cpp — the .xz transform.
//
// The transform has one job: an .xz stream is inflated, and the stream is rebuilt from a recipe
// read out of its own block header — no stored original, no diff. So the tests are about the two
// ways that can go wrong: the transform does not fire when it should (a silent no-op that costs
// ratio and looks fine), and it fires when it should not (wrong bytes, which the digest would
// catch but which should never be produced).
//
// The fixtures are built here with liblzma rather than checked in, because the point is the
// encoder's own output and a stale binary fixture would stop testing that.
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"

#include "chimera/block.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "precompressors/xz_wrap.hpp"

#include <lzma.h>

using namespace chce;

namespace {

// One .xz stream at a given preset, made with the library the transform will rebuild it with.
std::vector<uint8_t> make_xz(const std::vector<uint8_t>& raw, uint32_t preset) {
  std::vector<uint8_t> out;
  lzma_options_lzma opt;
  if (lzma_lzma_preset(&opt, preset)) return out;
  lzma_filter filters[2];
  filters[0].id = LZMA_FILTER_LZMA2;
  filters[0].options = &opt;
  filters[1].id = LZMA_VLI_UNKNOWN;
  filters[1].options = nullptr;

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK) return out;
  strm.next_in = raw.data();
  strm.avail_in = raw.size();
  uint8_t buf[1 << 16];
  lzma_ret ret = LZMA_OK;
  while (ret == LZMA_OK) {
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    ret = lzma_code(&strm, LZMA_FINISH);
    const size_t got = sizeof(buf) - strm.avail_out;
    out.insert(out.end(), buf, buf + got);
  }
  lzma_end(&strm);
  if (ret != LZMA_STREAM_END) out.clear();
  return out;
}

std::vector<uint8_t> prose(size_t n) {
  static const char* kWords[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
                                 "and", "then", "walks", "back", "again", "through", "the", "field"};
  std::vector<uint8_t> out;
  out.reserve(n);
  uint32_t x = 12345;
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

TEST(an_xz_stream_is_recognised) {
  const auto raw = prose(4096);
  const auto xz = make_xz(raw, 6);
  CHECK(!xz.empty());
  CHECK(xz_likely(xz));

  std::vector<uint8_t> not_xz = {0xFD, '7', 'z', 'X', 'Z', 0x01};
  CHECK(!xz_likely(not_xz));
  CHECK(!xz_likely(std::vector<uint8_t>{}));
}

// The recipe is the whole transform: if it does not reproduce the stream on its own, nothing
// else matters.
TEST(the_payload_rebuilds_the_stream_exactly) {
  const auto raw = prose(20000);
  for (uint32_t preset : {0u, 6u, 9u, 9u | LZMA_PRESET_EXTREME}) {
    const auto xz = make_xz(raw, preset);
    CHECK(!xz.empty());

    std::vector<uint8_t> payload;
    CHECK_MSG(xz_unpack(xz, payload), "preset " + std::to_string(preset) + " was not unpacked");

    std::vector<uint8_t> again;
    CHECK(xz_repack(payload, again));
    CHECK_MSG(again == xz, "preset " + std::to_string(preset) + " did not rebuild byte-exactly");
    // The payload is the inflated bytes plus a handful of numbers, not a copy of the stream.
    CHECK(payload.size() >= raw.size());
    CHECK(payload.size() < raw.size() + 32);
  }
}

// A stream that is not reproducible from its own header must be refused, not guessed at.
TEST(a_stream_that_does_not_reproduce_is_refused) {
  const auto raw = prose(20000);
  auto xz = make_xz(raw, 6);
  CHECK(!xz.empty());
  // Flip a byte in the compressed body: it may not even decode, and if it does, the rebuilt
  // stream cannot match.
  xz[xz.size() / 2] ^= 0x5A;
  std::vector<uint8_t> payload;
  CHECK_MSG(!xz_unpack(xz, payload), "a damaged stream was accepted");
  CHECK(payload.empty());

  // A payload whose recipe is nonsense is refused too.
  std::vector<uint8_t> bad = {'X', 'Z', '0', '1', 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::vector<uint8_t> out;
  CHECK(!xz_repack(bad, out));
}

// The whole point: a .tar.xz must come out smaller than it went in, with the layer bit set,
// and decode back to the original file.
TEST(a_tar_xz_compresses_and_round_trips) {
  const auto raw = prose(120000);
  const auto xz = make_xz(raw, 6);
  CHECK(!xz.empty());

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(xz, packed, precomp_on(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == xz, "round trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.layers3 & kLayer3Xz) != 0, "the xz layer bit was not set");

  // It has to be worth it: the archive must beat storing the .xz as opaque bytes.
  CompressOptions plain = precomp_on();
  plain.precomp = false;
  std::vector<uint8_t> stored;
  CHECK_ERR(compress(xz, stored, plain, nullptr), Err::Ok);
  CHECK_MSG(packed.size() < stored.size(),
            "the transform did not help: " + std::to_string(packed.size()) + " against " +
                std::to_string(stored.size()));
}

// What is inside an .xz is as reachable as what is inside a .gz. It was not: the seven nested
// transform gates read `(precomp || gzip)` and the xz stage was added later, so a `.tar.xz`
// holding a CSV got no numeric transform at all. Found by reading the gates after the transform
// shipped, not by a test — hence this one.
TEST(an_xz_payload_reaches_the_nested_transforms) {
  // A numeric CSV, which is what the numeric layer exists for, compressed inside an .xz.
  std::string csv = "id,value,count\n";
  long a = 1000, b = 500000, c = 0;
  for (int i = 0; i < 4000; ++i) {
    a += 3;
    b += i % 7;
    c += 1;
    csv += std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c) + "\n";
  }
  const std::vector<uint8_t> raw(csv.begin(), csv.end());
  const auto xz = make_xz(raw, 6);
  CHECK(!xz.empty());

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(xz, packed, precomp_on(), nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == xz, "round trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  CHECK_MSG((fh.layers3 & kLayer3Xz) != 0, "the xz layer bit was not set");
  // Either bit counts: a nested transform sets the `_nested` variant, and which one fires
  // depends on the payload, not on the container it arrived in.
  CHECK_MSG((fh.layers & (kLayerNumeric | kLayerNumericNested)) != 0,
            "the numeric layer did not fire inside the .xz, so the nested gates are still shut");
}

TEST_MAIN()
