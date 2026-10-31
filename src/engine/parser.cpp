// parser.cpp — shortest-path optimal parse over one block.
//
// The DP is the standard one from LZMA's optimal parser: cost[j] is the
// cheapest way to reach position j, and every candidate token relaxes the
// position it lands on. What is different here is that the price function is
// an object, so literals, fine matches and (later) coarse matches are all
// priced by the same code rather than by three special cases.
#include "parser.hpp"

#include <algorithm>
#include <limits>

namespace chce {
namespace {

constexpr uint32_t kInf = std::numeric_limits<uint32_t>::max();

}  // namespace

uint64_t parse_block(std::span<const uint8_t> data, uint32_t begin, uint32_t end,
                     MatchFinder& finder, const CostModel& model,
                     std::vector<Token>& out, const ParseOptions& opt) {
  if (end <= begin) return 0;
  const uint32_t n = end - begin;

  std::vector<uint32_t> cost(n + 1, kInf);
  std::vector<uint32_t> back_len(n + 1, 0);
  std::vector<uint32_t> back_dist(n + 1, 0);
  cost[0] = 0;

  auto relax = [&](uint32_t at, uint32_t c, uint32_t len, uint32_t dist) {
    if (c < cost[at]) {
      cost[at] = c;
      back_len[at] = len;
      back_dist[at] = dist;
    }
  };

  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t here = cost[i];
    const uint32_t pos = begin + i;

    // A literal is always available, so every position is reachable and the
    // finder sees every position exactly once.
    const uint32_t lit = model.literal_price(
        StaticCostModel::context_for(data, pos), data[pos]);
    relax(i + 1, here + lit, 1, 0);

    const std::span<const Match> matches = finder.query(pos);
    if (matches.empty()) continue;

    const uint32_t limit = std::min(opt.max_len, n - i);
    uint32_t prev_len = 1;  // the literal transition already covered length 1
    for (const Match& m : matches) {
      const uint32_t reach = std::min(m.len, limit);
      // Each match entry is priced as "reached this length at this distance",
      // so every length between the previous entry and this one is a separate
      // transition — that is where the parser chooses how far to extend.
      for (uint32_t len = prev_len + 1; len <= reach; ++len) {
        const uint32_t price = model.match_price(len, m.dist, MatchSource::Fine);
        relax(i + len, here + price, len, m.dist);
      }
      if (reach > prev_len) prev_len = reach;
      if (prev_len >= limit) break;
    }
  }

  // Walk the back-pointers to recover the cheapest path.
  out.clear();
  uint32_t j = n;
  while (j > 0) {
    const uint32_t len = back_len[j];
    if (len == 0) break;  // unreachable; cannot happen, but do not spin
    out.push_back(Token{len, back_dist[j]});
    j -= len;
  }
  std::reverse(out.begin(), out.end());
  return cost[n];
}

uint64_t parse_greedy(std::span<const uint8_t> data, uint32_t begin, uint32_t end,
                      MatchFinder& finder, const CostModel& model,
                      std::vector<Token>& out, const ParseOptions& opt) {
  out.clear();
  uint32_t pos = begin;
  while (pos < end) {
    const uint32_t limit = std::min(opt.max_len, end - pos);
    const std::span<const Match> matches = finder.query(pos);

    uint32_t best_len = 0;
    uint32_t best_dist = 0;
    if (!matches.empty() && limit >= 3) {
      // Longest match, and only worth taking if it is at least three bytes —
      // the classic greedy rule.
      const Match& m = matches.back();
      if (m.len >= 3) {
        best_len = std::min(m.len, limit);
        best_dist = m.dist;
      }
    }

    if (best_len >= 3) {
      out.push_back(Token{best_len, best_dist});
      finder.skip_to(pos + best_len);
      pos += best_len;
    } else {
      out.push_back(Token{1, 0});
      ++pos;
    }
  }
  return cost_of_tokens(data, begin, out, model);
}

uint64_t cost_of_tokens(std::span<const uint8_t> data, uint32_t begin,
                        const std::vector<Token>& tokens, const CostModel& model) {
  uint64_t total = 0;
  uint32_t pos = begin;
  for (const Token& t : tokens) {
    if (t.is_literal()) {
      total += model.literal_price(StaticCostModel::context_for(data, pos), data[pos]);
      ++pos;
    } else {
      total += model.match_price(t.len, t.dist, MatchSource::Fine);
      pos += t.len;
    }
  }
  return total;
}

}  // namespace chce
