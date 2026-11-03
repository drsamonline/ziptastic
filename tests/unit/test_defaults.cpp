// test_defaults.cpp — what the DEFAULT CompressOptions actually does.
//
// Written during the audit (docs/AUDIT_PLAN.md, D2) after the same wrong belief was
// written down three times: that `precomp = false` means "the library runs no Layer 0
// transform". It does not. It means the *container* stages are off. Every content
// transform — base64, numeric, wav, bmp, ecc, png, sqlite, jpeg, gif — defaults to ON.
//
// That distinction matters to anyone embedding the engine, and it has now been misstated
// in prose more than once, so it is pinned here in executable form: this test is the
// documentation of the default, and it fails if the default changes.
#include <cstring>
#include <vector>

#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

FileHeader compress_and_read(const std::vector<uint8_t>& in, const CompressOptions& opt) {
  std::vector<uint8_t> packed;
  CHECK_ERR(compress(in, packed, opt, nullptr), Err::Ok);
  std::vector<uint8_t> back;
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == in, "default-options round trip differs");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);
  return fh;
}

// A document whose payload is one long base64 run — the shape a JSON API response has.
std::vector<uint8_t> base64_document() {
  std::vector<uint8_t> v;
  const char* b64 =
      "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";
  for (int i = 0; i < 40; ++i) {
    v.insert(v.end(), b64, b64 + std::strlen(b64));
    v.push_back('\n');
  }
  return v;
}

}  // namespace

// The content transforms are on by default: a base64 payload is decoded without anyone
// asking for it.
TEST(default_options_decode_base64) {
  const auto fh = compress_and_read(base64_document(), CompressOptions{});
  CHECK_MSG((fh.layers & kLayerBase64) != 0,
            "base64 did not run under default options — the default has changed");
}

// The container transforms are OFF by default: a gzip member is not inflated unless the
// caller asks for `precomp`.
TEST(default_options_do_not_touch_containers) {
static const uint8_t kGzText[] = {
  0x1f, 0x8b, 0x08, 0x00, 0x10, 0xb2, 0xe9, 0x6a, 0x02, 0xff, 0xed, 0xca,
  0xdb, 0x11, 0x82, 0x30, 0x14, 0x00, 0xd1, 0x56, 0x6e, 0x05, 0xf4, 0xa4,
  0x10, 0x04, 0x51, 0xa2, 0x21, 0xc1, 0x47, 0xf5, 0x3a, 0x56, 0xe1, 0xc7,
  0xf9, 0xdc, 0xd9, 0x53, 0xa7, 0x14, 0xf7, 0x36, 0xf7, 0x4b, 0x1c, 0x4b,
  0x7e, 0xac, 0x31, 0xe6, 0x67, 0x9c, 0xdb, 0xf5, 0xb6, 0x45, 0xde, 0x53,
  0x89, 0xfa, 0xdd, 0x97, 0xc3, 0xfb, 0x15, 0x43, 0x3e, 0x75, 0xbf, 0x82,
  0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86,
  0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86,
  0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86,
  0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86, 0x61, 0x18, 0x86,
  0x61, 0x18, 0x86, 0xff, 0x12, 0x7f, 0x00, 0x12, 0x7f, 0x7f, 0x1c, 0x50,
  0x46, 0x00, 0x00
};

  // A whole file that IS a gzip member of repetitive text: the shape the stage is for.
  // An earlier version of this test buried a 30-byte member in 2 KB of random bytes and
  // the gate correctly declined it — inflating it would have gained nothing — so the
  // fixture was testing the gate, not the default.
  std::vector<uint8_t> in(kGzText, kGzText + sizeof(kGzText));

  const auto off = compress_and_read(in, CompressOptions{});
  CHECK_MSG((off.layers & kLayerGzip) == 0,
            "gzip ran under default options — the default has changed");

  CompressOptions on;
  on.precomp = true;
  const auto with = compress_and_read(in, on);
  CHECK_MSG((with.layers & kLayerGzip) != 0,
            "precomp = true did not enable the container stage");
}

// The two options that default to false, and the reason each is a deliberate choice
// rather than an oversight.
TEST(the_two_off_by_default_options_are_named) {
  const CompressOptions d{};
  CHECK_MSG(d.precomp == false, "precomp default changed");
  CHECK_MSG(d.coarse == false, "coarse default changed");
  // Everything that reads the input's FORMAT is on; only the two that cost the most
  // time per byte are off.
  CHECK_MSG(d.base64 && d.numeric && d.wav && d.bmp && d.ecc && d.sqlite && d.jpeg &&
                d.gif && d.dedup,
            "a content transform default changed");
}

TEST_MAIN()
