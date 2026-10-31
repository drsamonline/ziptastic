// test_parser.cpp — Phase 3 exit tests for the optimal parse.
//
// The oracle is a second, independently written shortest-path search: a
// memoised recursion over a pre-computed match table. If the iterative DP and
// the recursion agree on the cost of every small input, the DP is right.
#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

#include "chimera/cost_model.hpp"
#include "chimera/match_finder.hpp"
#include "engine/parser.hpp"
#include "engine/token_stream.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> make(uint32_t n, uint32_t alphabet, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) b = static_cast<uint8_t>('a' + rng.below(alphabet));
  return d;
}

// Match table for a whole buffer, gathered in one forward pass.
std::vector<std::vector<Match>> gather_matches(std::span<const uint8_t> data,
                                               uint32_t nice_len) {
  MatchFinder::Options opt;
  opt.window = 4096;
  opt.nice_len = nice_len;
  opt.depth = 100000;
  MatchFinder mf(data, opt);
  std::vector<std::vector<Match>> table(data.size());
  for (uint32_t p = 0; p < data.size(); ++p) {
    auto ms = mf.query(p);
    table[p].assign(ms.begin(), ms.end());
  }
  return table;
}

// Oracle: memoised recursion over the same transitions.
uint64_t oracle_cost(std::span<const uint8_t> data,
                     const std::vector<std::vector<Match>>& table,
                     const CostModel& model, uint32_t begin, uint32_t end) {
  const uint32_t n = end - begin;
  std::vector<uint64_t> memo(n + 1, ~0ull);
  std::function<uint64_t(uint32_t)> solve = [&](uint32_t i) -> uint64_t {
    if (i == n) return 0;
    if (memo[i] != ~0ull) return memo[i];
    const uint32_t pos = begin + i;
    uint64_t best =
        model.literal_price(StaticCostModel::context_for(data, pos), data[pos]) +
        solve(i + 1);
    uint32_t prev_len = 1;
    const uint32_t limit = std::min<uint32_t>(kMaxMatchLen, n - i);
    for (const Match& m : table[pos]) {
      const uint32_t reach = std::min(m.len, limit);
      for (uint32_t len = prev_len + 1; len <= reach; ++len) {
        const uint64_t c =
            model.match_price(len, m.dist, MatchSource::Fine) + solve(i + len);
        if (c < best) best = c;
      }
      if (reach > prev_len) prev_len = reach;
      if (prev_len >= limit) break;
    }
    memo[i] = best;
    return best;
  };
  return solve(0);
}

}  // namespace

TEST(agrees_with_independent_oracle) {
  for (uint32_t alphabet : {2u, 4u, 16u}) {
    for (uint64_t seed : {1ull, 2ull, 3ull}) {
      auto data = make(1200, alphabet, seed);
      StaticCostModel model(data);
      auto table = gather_matches(data, kMaxMatchLen);

      MatchFinder::Options opt;
      opt.window = 4096;
      opt.nice_len = kMaxMatchLen;
      opt.depth = 100000;
      MatchFinder mf(data, opt);

      std::vector<Token> tokens;
      const uint64_t dp = parse_block(data, 0, static_cast<uint32_t>(data.size()),
                                      mf, model, tokens);
      const uint64_t oracle =
          oracle_cost(data, table, model, 0, static_cast<uint32_t>(data.size()));
      CHECK_MSG(dp == oracle, "alphabet " + std::to_string(alphabet) + " seed " +
                                  std::to_string(seed) + ": dp " +
                                  std::to_string(dp) + " vs oracle " +
                                  std::to_string(oracle));
      // The reported cost must match the cost of the tokens it returned.
      CHECK_EQ(cost_of_tokens(data, 0, tokens, model), dp);
    }
  }
}

TEST(tokens_describe_the_input) {
  auto data = make(4000, 5, 9);
  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

  uint32_t pos = 0;
  for (const Token& t : tokens) {
    CHECK(t.len >= 1);
    if (t.is_literal()) {
      CHECK_EQ(t.dist, uint32_t{0});
      ++pos;
    } else {
      CHECK(t.len >= kMinMatchLen);
      CHECK(t.len <= kMaxMatchLen);
      CHECK(t.dist >= 1);
      CHECK(t.dist <= pos);
      if (t.dist <= pos && pos + t.len <= data.size()) {
        CHECK_MSG(std::memcmp(&data[pos - t.dist], &data[pos], t.len) == 0,
                  "token at " + std::to_string(pos) + " does not match");
      }
      pos += t.len;
    }
  }
  CHECK_EQ(pos, static_cast<uint32_t>(data.size()));
}

TEST(round_trip_through_token_stream) {
  for (uint32_t alphabet : {2u, 8u, 256u}) {
    auto data = make(3000, alphabet, 42 + alphabet);
    StaticCostModel model(data);
    MatchFinder mf(data);
    std::vector<Token> tokens;
    parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

    ByteWriter w;
    encode_tokens(w, data, 0, tokens);

    std::vector<uint8_t> out;
    ByteReader r(w.span());
    CHECK_ERR(decode_tokens(r, static_cast<uint32_t>(data.size()), out), Err::Ok);
    CHECK(r.at_end());
    CHECK_MSG(out == data, "token stream round-trip failed at alphabet " +
                               std::to_string(alphabet));
  }
}

TEST(multi_block_parse_carries_the_window) {
  // Parse in two halves with one finder: matches in the second half may point
  // back into the first, and the token stream must still round-trip because
  // the decoder expands into one growing buffer.
  //
  // The second half is a copy of the first, so the *only* way to code it well
  // is to reference across the block boundary — a repetitive fixture would let
  // the parser stay local and prove nothing.
  auto first_half = make(3000, 256, 77);
  auto data = first_half;
  data.insert(data.end(), first_half.begin(), first_half.end());
  StaticCostModel model(data);
  MatchFinder mf(data);

  std::vector<Token> first, second;
  parse_block(data, 0, 3000, mf, model, first);
  parse_block(data, 3000, 6000, mf, model, second);

  // At least one token in the second block must reach before 3000, or this
  // test is not exercising the carry at all.
  uint32_t pos = 3000;
  bool reaches_back = false;
  for (const Token& t : second) {
    if (!t.is_literal() && t.dist > pos - 3000) reaches_back = true;
    pos += t.len;
  }
  CHECK_MSG(reaches_back, "second block never referenced the first");

  ByteWriter w;
  encode_tokens(w, data, 0, first);
  encode_tokens(w, data, 3000, second);

  std::vector<uint8_t> out;
  ByteReader r(w.span());
  CHECK_ERR(decode_tokens(r, 3000, out), Err::Ok);
  CHECK_ERR(decode_tokens(r, 3000, out), Err::Ok);
  CHECK(out == data);
}

TEST(optimal_beats_greedy) {
  // The whole point of the DP: it must never be worse than greedy, and on
  // compressible data it must be meaningfully better.
  auto data = make(20000, 4, 5);
  StaticCostModel model(data);

  MatchFinder::Options opt;
  opt.window = 4096;
  opt.nice_len = kMaxMatchLen;
  opt.depth = 100000;

  std::vector<Token> best, greedy;
  MatchFinder mf1(data, opt);
  const uint64_t c_opt = parse_block(data, 0, 20000, mf1, model, best);
  MatchFinder mf2(data, opt);
  const uint64_t c_greedy = parse_greedy(data, 0, 20000, mf2, model, greedy);

  CHECK_MSG(c_opt <= c_greedy, "optimal " + std::to_string(c_opt) +
                                   " worse than greedy " +
                                   std::to_string(c_greedy));
  const double gain = c_greedy ? 100.0 * (static_cast<double>(c_greedy) - c_opt) /
                                     static_cast<double>(c_greedy)
                               : 0.0;
  CHECK_MSG(gain > 1.0, "optimal only " + std::to_string(gain) +
                            "% better than greedy on 4-letter data");
}

TEST(incompressible_input_is_all_literals) {
  chce_test::Rng rng(99);
  std::vector<uint8_t> data(4000);
  for (auto& b : data) b = rng.byte();
  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, 4000, mf, model, tokens);

  // Random data has almost no repeats; whatever matches exist must be rare.
  uint32_t matched = 0;
  for (const Token& t : tokens) {
    if (!t.is_literal()) matched += t.len;
  }
  CHECK_MSG(matched < 400, "matched " + std::to_string(matched) +
                               " bytes of random data");
}

TEST(round_trip_with_partial_trailing_block) {
  // A block boundary that does not divide the input leaves a short final block
  // with a small len_limit — the region where the finder's tail behaviour and
  // the token stream's cross-block references both get exercised.
  auto data = make(700000, 4, 23);
  StaticCostModel model(data);
  MatchFinder mf(data);

  const uint32_t bs = 256 * 1024;
  std::vector<Token> tokens;
  std::vector<std::pair<uint32_t, uint32_t>> blocks;
  for (uint32_t pos = 0; pos < data.size(); pos += bs) {
    const uint32_t end = std::min<uint32_t>(pos + bs, static_cast<uint32_t>(data.size()));
    tokens.clear();
    parse_block(data, pos, end, mf, model, tokens);
    blocks.emplace_back(pos, end);
  }

  // Re-parse and stream every block through encode/decode as the engine does.
  MatchFinder mf2(data);
  ByteWriter w;
  for (auto [pos, end] : blocks) {
    tokens.clear();
    parse_block(data, pos, end, mf2, model, tokens);
    encode_tokens(w, data, pos, tokens);
  }

  std::vector<uint8_t> out;
  ByteReader r(w.span());
  for (auto [pos, end] : blocks) {
    CHECK_ERR(decode_tokens(r, end - pos, out), Err::Ok);
  }
  CHECK(r.at_end());
  CHECK_MSG(out == data, "multi-block round-trip with a partial tail failed");
}

TEST(cost_model_is_monotone) {
  auto data = make(2000, 8, 3);
  StaticCostModel model(data);
  // A longer match never costs more than a shorter one at the same distance.
  for (uint32_t len = 2; len < 64; ++len) {
    CHECK(model.match_price(len + 1, 100, MatchSource::Fine) >=
          model.match_price(len, 100, MatchSource::Fine));
  }
  // A closer match never costs more than a distant one of the same length.
  for (uint32_t dist = 1; dist < 100000; dist *= 2) {
    CHECK(model.match_price(8, dist * 2, MatchSource::Fine) >=
          model.match_price(8, dist, MatchSource::Fine));
  }
  // Every literal has a finite, positive price.
  for (uint32_t b = 0; b < 256; ++b) {
    CHECK(model.literal_price(0, static_cast<uint8_t>(b)) > 0);
  }
}

TEST_MAIN()
