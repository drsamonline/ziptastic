// parser.hpp — the optimal (shortest-path) parse.
//
// One DP over positions, one cost model, one match list per position: the
// parser prices literals, fine matches and (from Phase 6) coarse matches with
// the same function, which is what makes it a *joint* parser rather than a
// chain of special cases.
//
// Tokens are the output: a literal is a token of length 1, a match carries a
// length and a backward distance.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/cost_model.hpp"
#include "chimera/match_finder.hpp"

namespace chce {

struct Token {
  uint32_t len;   // 1 for a literal
  uint32_t dist;  // 0 for a literal

  bool is_literal() const { return len == 1; }
};

struct ParseOptions {
  // Matches longer than this are not considered (the format's limit).
  uint32_t max_len = kMaxMatchLen;
  // Hard cap on how far ahead the DP may reach; only matters for pathological
  // inputs, since a token never spans more than max_len bytes.
  uint32_t max_tokens = 1u << 20;
};

// Parses `data[begin, end)` and appends tokens to `out`.
//
// `finder` must already have been advanced to at least `begin` and must be
// queried in increasing position order — the parser walks positions once, left
// to right, and the index is built as a side effect of that walk. The finder
// is left positioned at `end`, so the next block continues where this one
// stopped and the dictionary carries across blocks, as the format requires.
//
// Returns the modelled cost of the parse in 1/16 bit units.
uint64_t parse_block(std::span<const uint8_t> data, uint32_t begin, uint32_t end,
                     MatchFinder& finder, const CostModel& model,
                     std::vector<Token>& out, const ParseOptions& opt = {});

// A greedy parse: take the longest match at each position, else a literal.
// Not used by the encoder — it exists so tests and benchmarks can show what the
// DP is actually buying over the obvious strategy.
uint64_t parse_greedy(std::span<const uint8_t> data, uint32_t begin, uint32_t end,
                      MatchFinder& finder, const CostModel& model,
                      std::vector<Token>& out, const ParseOptions& opt = {});

// Total modelled cost of an existing token list, for comparisons.
uint64_t cost_of_tokens(std::span<const uint8_t> data, uint32_t begin,
                        const std::vector<Token>& tokens, const CostModel& model);

}  // namespace chce
