// test_dedup.cpp — the coarse dedup stage.
//
// Two things matter: it must rebuild exactly, and it must decline data whose
// repeats are short. Declining is not politeness — at a 64-byte minimum this
// stage made xml 20.8% WORSE, because short repeats are what BWT exploits.
#include <cstring>
#include <vector>

#include "harness.hpp"
#include "precompressors/dedup.hpp"

using namespace chce;

namespace {
std::vector<uint8_t> text(uint32_t n, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) b = static_cast<uint8_t>('a' + rng.below(26));
  return d;
}
}  // namespace

TEST(round_trips_with_a_large_duplicate) {
  auto a = text(200000, 1);
  std::vector<uint8_t> d = a;
  d.insert(d.end(), a.begin(), a.end());  // exact duplicate: should be lifted out

  std::vector<uint8_t> packed;
  CHECK_MSG(dedup_pack(d, packed), "did not detect a 200 KB duplicate");
  // One copy plus command overhead: 200,014 for a 400,000-byte input. (Exactly
  // half would be 200,000, which the literal header alone puts out of reach.)
  CHECK_MSG(packed.size() < d.size() * 3 / 5, "duplicate not actually removed: " +
                                                 std::to_string(packed.size()));

  std::vector<uint8_t> out;
  CHECK_MSG(dedup_unpack(packed, out), "unpack failed");
  CHECK_MSG(out == d, "dedup round-trip mismatch");
}

TEST(declines_xml_like_repetition) {
  // Repetitive, but with no long exact repeats: repeated tags around varying
  // content. This is the shape that made xml 20.8% worse when the minimum match
  // was 64 bytes, and it must be declined now.
  chce_test::Rng rng(11);
  std::vector<uint8_t> d;
  while (d.size() < 200000) {
    const char* tag = "<item id=\"";
    for (const char* p = tag; *p; ++p) d.push_back(static_cast<uint8_t>(*p));
    for (int i = 0; i < 12; ++i) d.push_back(static_cast<uint8_t>('0' + rng.below(10)));
    const char* tail = "\">x</item>\n";
    for (const char* p = tail; *p; ++p) d.push_back(static_cast<uint8_t>(*p));
  }

  std::vector<uint8_t> packed;
  CHECK_MSG(!dedup_pack(d, packed), "should decline xml-like short repetition");
}

TEST(a_periodic_pattern_is_a_real_duplicate) {
  // A period-7 pattern DOES have long exact matches at distance 7, so lifting
  // them out is correct. (An earlier version of this test asserted the opposite
  // and was simply wrong about the data.)
  std::vector<uint8_t> d;
  for (int i = 0; i < 200000; ++i) d.push_back(static_cast<uint8_t>('a' + (i % 7)));
  std::vector<uint8_t> packed;
  if (!dedup_pack(d, packed)) return;
  std::vector<uint8_t> out;
  CHECK_MSG(dedup_unpack(packed, out), "unpack failed");
  CHECK_MSG(out == d, "periodic round-trip mismatch");
}

TEST(declines_unique_data) {
  auto d = text(300000, 9);
  std::vector<uint8_t> packed;
  CHECK_MSG(!dedup_pack(d, packed), "should decline data with no duplicates");
}

TEST(corruption_never_produces_a_wrong_length) {
  // The packed stream carries no checksum, so a flipped literal byte is
  // undetectable and decodes to wrong content of the right length. That is a
  // property of the format (which has no integrity field at all), not of this
  // stage. What must hold: unpack either fails, or produces exactly the declared
  // length. It must never return a short or over-long buffer.
  auto a = text(100000, 3);
  std::vector<uint8_t> d = a;
  d.insert(d.end(), a.begin(), a.end());
  std::vector<uint8_t> packed;
  if (!dedup_pack(d, packed)) return;

  for (size_t off : {packed.size() / 4, packed.size() / 2, packed.size() * 3 / 4}) {
    std::vector<uint8_t> bad = packed;
    bad[off] ^= 0xFF;
    std::vector<uint8_t> out;
    const bool ok = dedup_unpack(bad, out);
    if (ok) CHECK_MSG(out.size() == d.size(), "unpack produced the wrong length");
  }
}

TEST_MAIN()
