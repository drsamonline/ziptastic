// test_match_finder.cpp — Phase 2 exit tests for the BT4 finder.
//
// The authority here is brute force: for any position, a match finder must
// never miss a match that an exhaustive scan of the same window would find.
// Everything else (ordering, distance validity, window bounds, determinism)
// is checked against the same scan.
#include <algorithm>
#include <cstring>

#include "chimera/match_finder.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

// Exhaustive scan: the longest match available at `pos` within `window`
// bytes back, capped at `max_len`. This is the ground truth.
uint32_t brute_best(const std::vector<uint8_t>& d, uint32_t pos, uint32_t window,
                    uint32_t max_len) {
  uint32_t best = 0;
  if (pos == 0) return 0;
  const uint32_t first = pos > window - 1 ? pos - (window - 1) : 0;
  for (uint32_t p = first; p < pos; ++p) {
    uint32_t len = 0;
    while (len < max_len && pos + len < d.size() && d[p + len] == d[pos + len]) {
      ++len;
    }
    best = std::max(best, len);
  }
  return best;
}

std::vector<uint8_t> make(uint32_t n, uint32_t alphabet, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) b = static_cast<uint8_t>('a' + rng.below(alphabet));
  return d;
}

// Every reported match must be real: same bytes, same distance, and inside the
// length cap. A finder that invents matches would corrupt every stream it
// feeds, so this is checked on every list in every test below.
void check_valid(const std::vector<uint8_t>& d, uint32_t pos,
                 std::span<const Match> ms, uint32_t len_limit) {
  uint32_t last_len = 0;
  for (const Match& m : ms) {
    CHECK_MSG(m.dist >= 1 && m.dist <= pos,
              "distance " + std::to_string(m.dist) + " at pos " +
                  std::to_string(pos));
    CHECK_MSG(m.len >= 2, "match shorter than 2 bytes");
    CHECK_MSG(m.len <= len_limit,
              "match len " + std::to_string(m.len) + " over limit " +
                  std::to_string(len_limit));
    CHECK_MSG(m.len > last_len, "matches must be ordered by increasing length");
    last_len = m.len;
    if (m.dist <= pos && pos + m.len <= d.size()) {
      CHECK_MSG(std::memcmp(&d[pos - m.dist], &d[pos], m.len) == 0,
                "reported match does not actually match at pos " +
                    std::to_string(pos));
    }
  }
}

}  // namespace

TEST(known_strings) {
  // A single repeated character: every position has a long match at distance 1.
  {
    std::vector<uint8_t> d(64, 'x');
    MatchFinder mf(d);
    auto ms = mf.query(32);
    CHECK(!ms.empty());
    CHECK_EQ(ms.back().dist, uint32_t{1});
    check_valid(d, 32, ms, 64);
  }
  // "abcabcabc...": at pos 3 the best match is distance 3, length 9.
  {
    std::vector<uint8_t> d;
    for (int i = 0; i < 12; ++i) d.push_back("abc"[i % 3]);
    MatchFinder mf(d);
    auto ms = mf.query(3);
    CHECK(!ms.empty());
    CHECK_EQ(ms.back().dist, uint32_t{3});
    CHECK_EQ(ms.back().len, uint32_t{9});
    check_valid(d, 3, ms, 12);
  }
  // No repetition at all: no matches anywhere. The ramp must stay under 256
  // bytes — past that the byte values wrap and "0,1" genuinely repeats.
  {
    std::vector<uint8_t> d;
    for (int i = 0; i < 200; ++i) d.push_back(static_cast<uint8_t>(i));
    MatchFinder mf(d);
    for (uint32_t p = 0; p < 200; ++p) {
      auto ms = mf.query(p);
      // A 2-byte match is impossible in a strict ramp; nothing may be reported.
      CHECK_MSG(ms.empty(), "phantom match at pos " + std::to_string(p));
    }
  }
}

TEST(matches_brute_force_small) {
  // Exhaustive: every position of several small inputs, full window.
  for (uint32_t alphabet : {2u, 3u, 8u}) {
    for (uint64_t seed : {1ull, 2ull}) {
      auto d = make(3000, alphabet, seed);
      MatchFinder::Options opt;
      opt.window = 4096;
      opt.nice_len = 273;
      opt.depth = 100000;  // no cutoff: we want the true best
      MatchFinder mf(d, opt);
      const uint32_t window = mf.window_size();

      for (uint32_t pos = 0; pos < d.size(); ++pos) {
        auto ms = mf.query(pos);
        const uint32_t len_limit =
            std::min<uint32_t>(273, static_cast<uint32_t>(d.size() - pos));
        check_valid(d, pos, ms, len_limit);

        const uint32_t found = ms.empty() ? 0 : ms.back().len;
        const uint32_t truth = brute_best(d, pos, window, len_limit);
        // A match of 4+ bytes must be found exactly: those all share the
        // 4-byte prefix the tree is keyed on. Shorter ones may be missed
        // because the 2-/3-byte tables keep only the most recent position —
        // the same trade LZMA makes.
        if (truth >= 4) {
          CHECK_MSG(found == truth,
                    "pos " + std::to_string(pos) + ": found " +
                        std::to_string(found) + ", brute force " +
                        std::to_string(truth));
        }
        CHECK_MSG(found <= truth, "found a match longer than any that exists");
      }
    }
  }
}

TEST(matches_brute_force_sampled) {
  // Bigger input, sampled positions, narrower window.
  auto d = make(20000, 4, 7);
  MatchFinder::Options opt;
  opt.window = 1024;
  opt.nice_len = 273;
  opt.depth = 100000;
  MatchFinder mf(d, opt);
  const uint32_t window = mf.window_size();

  for (uint32_t pos = 0; pos < d.size(); pos += 7) {
    auto ms = mf.query(pos);
    const uint32_t len_limit =
        std::min<uint32_t>(273, static_cast<uint32_t>(d.size() - pos));
    check_valid(d, pos, ms, len_limit);
    const uint32_t found = ms.empty() ? 0 : ms.back().len;
    const uint32_t truth = brute_best(d, pos, window, len_limit);
    if (truth >= 4) CHECK_EQ(found, truth);
  }
}

TEST(window_bound_is_respected) {
  // A 40-byte needle, then filler, then the needle again.
  std::vector<uint8_t> d;
  const std::vector<uint8_t> needle(40, 'Z');
  d.insert(d.end(), needle.begin(), needle.end());
  const uint32_t filler = 5000;
  for (uint32_t i = 0; i < filler; ++i) d.push_back(static_cast<uint8_t>(i % 251));
  const uint32_t second = static_cast<uint32_t>(d.size());
  d.insert(d.end(), needle.begin(), needle.end());

  // Window smaller than the distance: the repeat must not be found.
  {
    MatchFinder::Options opt;
    opt.window = 4096;
    opt.nice_len = 273;
    MatchFinder mf(d, opt);
    mf.skip_to(second);
    auto ms = mf.query(second);
    for (const Match& m : ms) {
      CHECK_MSG(m.len < 40, "found a 40-byte match beyond the window");
    }
  }
  // Window larger than the distance: it must be found, at the right distance.
  {
    MatchFinder::Options opt;
    opt.window = 8192;
    opt.nice_len = 273;
    MatchFinder mf(d, opt);
    mf.skip_to(second);
    auto ms = mf.query(second);
    CHECK(!ms.empty());
    CHECK_EQ(ms.back().len, uint32_t{40});
    CHECK_EQ(ms.back().dist, second);
  }
}

TEST(nice_len_caps_matches) {
  std::vector<uint8_t> d(1000, 'q');
  MatchFinder::Options opt;
  opt.nice_len = 16;
  opt.depth = 100000;
  MatchFinder mf(d, opt);
  for (uint32_t pos = 0; pos < d.size(); pos += 37) {
    auto ms = mf.query(pos);
    for (const Match& m : ms) CHECK(m.len <= 16);
  }
}

TEST(skip_and_query_agree) {
  // Walking every position vs skipping ahead must give the same answer for the
  // positions that are actually queried: skipping still inserts.
  auto d = make(5000, 3, 11);
  MatchFinder::Options opt;
  opt.window = 4096;
  opt.nice_len = 273;
  opt.depth = 100000;

  MatchFinder a(d, opt);
  MatchFinder b(d, opt);
  for (uint32_t pos = 0; pos < d.size(); ++pos) {
    auto ma = a.query(pos);
    std::vector<Match> copy(ma.begin(), ma.end());
    if (pos % 100 == 0) {
      auto mb = b.query(pos);
      CHECK_MSG(copy.size() == mb.size(),
                "skip/query disagree on count at " + std::to_string(pos));
      for (size_t i = 0; i < std::min(copy.size(), mb.size()); ++i) {
        CHECK_EQ(copy[i].len, mb[i].len);
        CHECK_EQ(copy[i].dist, mb[i].dist);
      }
    } else {
      b.skip_to(pos + 1);
    }
  }
}

TEST(repeat_queries_are_safe) {
  auto d = make(2000, 4, 5);
  MatchFinder mf(d);
  auto first = mf.query(500);
  std::vector<Match> copy(first.begin(), first.end());
  // Asking again for the same position must not corrupt anything: the index
  // already contains it, so we refuse rather than re-insert.
  auto again = mf.query(500);
  CHECK(again.empty());
  auto later = mf.query(501);
  for (const Match& m : later) CHECK(m.dist >= 1 && m.dist <= 501);
  CHECK(!copy.empty());
}

TEST(deterministic) {
  auto d = make(8000, 5, 21);
  MatchFinder::Options opt;
  opt.window = 8192;
  MatchFinder a(d, opt);
  MatchFinder b(d, opt);
  for (uint32_t pos = 0; pos < d.size(); ++pos) {
    auto ma = a.query(pos);
    std::vector<Match> ca(ma.begin(), ma.end());
    auto mb = b.query(pos);
    CHECK_EQ(ca.size(), mb.size());
    for (size_t i = 0; i < std::min(ca.size(), mb.size()); ++i) {
      CHECK_EQ(ca[i].len, mb[i].len);
      CHECK_EQ(ca[i].dist, mb[i].dist);
    }
  }
}

TEST(memory_is_bounded_by_window) {
  // A 64 MiB window would be 512 MiB of tree; the finder must scale that down
  // to what the input can actually use.
  std::vector<uint8_t> small(100000, 'a');
  MatchFinder mf(small);
  CHECK(mf.window_size() <= 131072);
  CHECK(mf.memory_bytes() < 8u << 20);

  std::vector<uint8_t> big(3u << 20, 'b');
  MatchFinder::Options opt;
  opt.window = 64u << 20;
  MatchFinder mf2(big, opt);
  CHECK(mf2.window_size() >= (1u << 21));
  CHECK(mf2.memory_bytes() <= (1u << 26));
}

TEST(positions_inserted_tracks_progress) {
  auto d = make(1000, 4, 3);
  MatchFinder mf(d);
  mf.query(10);
  CHECK_EQ(mf.next_position(), uint32_t{11});
  mf.skip_to(500);
  CHECK_EQ(mf.next_position(), uint32_t{500});
  CHECK_EQ(mf.positions_inserted(), uint64_t{500});
}

TEST_MAIN()
