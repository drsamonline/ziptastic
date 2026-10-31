// test_coarse_index.cpp — Phase 6's index, on its own.
//
// The gate this phase has to clear: a file repeated after a large gap of
// incompressible data must be found by the coarse index. Everything reported
// must also be a real match — the index may miss, but it must never lie.
#include <cstring>
#include <vector>

#include "chimera/coarse_index.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> make(uint32_t n, uint32_t alphabet, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) b = static_cast<uint8_t>('a' + rng.below(alphabet));
  return d;
}

// Every reported match must be real.
void check_real(const std::vector<uint8_t>& d, uint32_t pos, const Match& m) {
  CHECK(m.dist >= 1 && m.dist <= pos);
  CHECK(m.len >= 1);
  if (m.dist <= pos && pos + m.len <= d.size()) {
    CHECK_MSG(std::memcmp(&d[pos - m.dist], &d[pos], m.len) == 0,
              "coarse match at " + std::to_string(pos) + " is not real");
  }
}

}  // namespace

TEST(finds_a_repeat_after_a_gap) {
  // The phase gate, in miniature: a block, a gap of noise, then the block again.
  auto a = make(40000, 256, 1);
  std::vector<uint8_t> noise = make(200000, 256, 2);
  std::vector<uint8_t> d = a;
  d.insert(d.end(), noise.begin(), noise.end());
  const uint32_t second = static_cast<uint32_t>(d.size());
  d.insert(d.end(), a.begin(), a.end());

  CoarseIndex idx(d);
  for (uint32_t p = 0; p + 48 <= d.size(); ++p) idx.insert(p);

  Match m{};
  bool found = false;
  // Look for the repeat a little way into the second copy.
  for (uint32_t p = second + 1000; p < second + 5000; ++p) {
    if (idx.query(p, &m)) {
      check_real(d, p, m);
      CHECK_EQ(m.dist, p - (p - second));  // must point into the first copy
      if (m.len >= 48) { found = true; break; }
    }
  }
  CHECK_MSG(found, "coarse index did not find the repeat across the gap");
}

TEST(no_false_matches_on_noise) {
  auto d = make(300000, 256, 7);
  CoarseIndex idx(d);
  for (uint32_t p = 0; p + 48 <= d.size(); ++p) idx.insert(p);

  int reported = 0;
  Match m{};
  for (uint32_t p = 0; p < d.size(); p += 7) {
    if (idx.query(p, &m)) {
      check_real(d, p, m);
      ++reported;
    }
  }
  // Random bytes can repeat by chance, but not often at 48 bytes.
  CHECK_MSG(reported < 50, "too many coarse matches on random data: " +
                               std::to_string(reported));
}

TEST(every_match_is_real_on_structured_data) {
  // Repetitive on purpose. Random 4-letter data has NO 48-byte repeats — a
  // 48-byte match in a 4-symbol alphabet needs (1/4)^40 by chance — so using it
  // here tested nothing and reported a false failure.
  std::vector<uint8_t> d;
  auto motif = make(2000, 4, 11);
  while (d.size() < 200000) {
    d.insert(d.end(), motif.begin(), motif.end());
  }
  CoarseIndex idx(d);
  int reported = 0;
  for (uint32_t p = 0; p + 48 <= d.size(); ++p) {
    idx.insert(p);
    Match m{};
    if (idx.query(p, &m)) {
      check_real(d, p, m);
      CHECK(m.len <= 273);
      ++reported;
    }
  }
  CHECK_MSG(reported > 0, "no coarse matches found on 4-letter data");
}

TEST(anchored_slots_survive_long_noise) {
  // The bucket policy is the whole point: a short anchor followed by a lot of
  // noise must still be findable, which pure round-robin would lose.
  auto a = make(20000, 256, 3);
  std::vector<uint8_t> noise = make(3000000, 256, 4);  // ~3 MB of noise
  std::vector<uint8_t> d = a;
  d.insert(d.end(), noise.begin(), noise.end());
  const uint32_t second = static_cast<uint32_t>(d.size());
  d.insert(d.end(), a.begin(), a.end());

  CoarseIndex idx(d);
  for (uint32_t p = 0; p + 48 <= d.size(); ++p) idx.insert(p);

  Match m{};
  int hits = 0;
  for (uint32_t p = second; p < second + 20000; p += 97) {
    if (idx.query(p, &m)) {
      check_real(d, p, m);
      ++hits;
    }
  }
  CHECK_MSG(hits > 10, "anchor policy lost the first copy after 3 MB of noise");
}

TEST(memory_is_bounded) {
  std::vector<uint8_t> d(1000000, 'x');
  CoarseIndex idx(d);
  CHECK(idx.memory_bytes() < (200u << 20));
  CHECK(idx.buckets() >= (1u << 12));
}

TEST_MAIN()
