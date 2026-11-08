// test_zpaq.cpp — the context-mixing backend must round-trip exactly.
#include <cstdio>
#include <cstring>
#include <vector>

#include "chimera/block.hpp"
#include "chimera/block_index.hpp"
#include "chimera/format.hpp"
#include "engine/compressor.hpp"
#include "engine/decompressor.hpp"
#include "entropy/cm_zpaq.hpp"
#include "harness.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> text(uint32_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  static const char* w[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over "};
  std::vector<uint8_t> d;
  while (d.size() < n) {
    const char* s = w[rng.below(6)];
    for (const char* p = s; *p; ++p) d.push_back(static_cast<uint8_t>(*p));
  }
  d.resize(n);
  return d;
}
}  // namespace

TEST(round_trips) {
  for (uint32_t n : {1024u, 16384u, 65536u}) {
    auto d = text(n, n);
    std::vector<uint8_t> c;
    if (!zpaq_compress_block(d, c)) continue;  // declined
    CHECK(c.size() < d.size());
    std::vector<uint8_t> out;
    CHECK_MSG(zpaq_decompress_block(c, n, out), "zpaq decode failed at " + std::to_string(n));
    CHECK_MSG(out == d, "zpaq round-trip failed at " + std::to_string(n));
  }
}

TEST(rejects_corruption) {
  auto d = text(8192, 3);
  std::vector<uint8_t> c;
  if (!zpaq_compress_block(d, c)) return;
  std::vector<uint8_t> bad = c;
  bad[bad.size() / 2] ^= 0xFF;
  std::vector<uint8_t> out;
  // Either it refuses, or it produces something that is not the original. What
  // it must not do is claim success with the wrong bytes.
  const bool ok = zpaq_decompress_block(bad, static_cast<uint32_t>(d.size()), out);
  if (ok) CHECK_MSG(out == d, "corrupt payload decoded to wrong bytes but reported success");
}

// The whole-stream CM block writes the archive header itself, and it wrote the wrong sizes for
// three years' worth of assumptions: `original_size = input.size()` when a Layer 0 transform had
// already shrunk the payload. The header then disagreed with the block, and reading the archive
// failed with "declared size does not match payload".
//
// The shape that exercises it is: a transform fires AND the CM block wins. It needs both — with the
// block path winning, the CM header is never written; with no transform, the two sizes are equal.
// Disabling the BWT candidate is enough to make CM win on numeric CSV, which is what this does.
TEST(the_cm_header_agrees_with_its_block_when_a_transform_fired) {
  // A numeric CSV, which is what the integer column transform exists for.
  // A ratio column as well: floats resist the integer transform and weaken the block path just
  // enough that the CM block wins, which is the whole point of the fixture.
  std::string csv = "id,value,count,ratio,label\n";
  long a = 1000, b = 500000, c = 0;
  char buf[128];
  for (int i = 0; i < 40000; ++i) {
    a += 3;
    b += i % 7;
    c += 1;
    std::snprintf(buf, sizeof(buf), "%ld,%ld,%ld,%.6f,row%d\n", a, b, c,
                  static_cast<double>(a) / static_cast<double>(b), i % 97);
    csv += buf;
  }
  const std::vector<uint8_t> raw(csv.begin(), csv.end());

  CompressOptions opt;
  opt.precomp = true;
  opt.coarse = true;
  opt.max_ratio = true;
  opt.bwt = false;  // without this the block path wins and the CM header is never written

  std::vector<uint8_t> packed, back;
  CHECK_ERR(compress(raw, packed, opt, nullptr), Err::Ok);
  CHECK_ERR(decompress(packed, back), Err::Ok);
  CHECK_MSG(back == raw, "round trip mismatch");

  ByteReader r(packed);
  FileHeader fh;
  CHECK_ERR(read_file_header(r, fh), Err::Ok);

  // The test is only testing this if the CM block is what got written. Without this assertion it
  // passes on the block path, where the CM header is never produced — which is exactly how the
  // control run failed to catch the bug it was written for.
  StreamIndex idx;
  CHECK_ERR(read_index(packed, idx), Err::Ok);
  CHECK_MSG(idx.size() == 1 && idx.blocks[0].mode == Mode::CmZpaq,
            "the CM block did not win, so the CM header was never written and this proves nothing");

  CHECK_MSG((fh.layers & kLayerNumeric) != 0,
            "the numeric transform did not fire, so this test is not testing what it says");
  CHECK_MSG(fh.original_size < fh.true_size,
            "a transform fired but original_size is not the post-transform size — the header is "
            "about to disagree with the block");
  CHECK_MSG(fh.true_size == raw.size(), "true_size is not the size of the input");
}

TEST_MAIN()
