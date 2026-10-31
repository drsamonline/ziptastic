// test_rans.cpp — the entropy layer on its own, then through the token coder.
#include <cstring>
#include <vector>

#include "chimera/cost_model.hpp"
#include "chimera/match_finder.hpp"
#include "engine/parser.hpp"
#include "entropy/rans.hpp"
#include "entropy/token_coder.hpp"
#include "harness.hpp"

using namespace chce;

namespace {

std::vector<uint8_t> make(uint32_t n, uint32_t alphabet, uint64_t seed) {
  chce_test::Rng rng(seed);
  std::vector<uint8_t> d(n);
  for (auto& b : d) b = static_cast<uint8_t>('a' + rng.below(alphabet));
  return d;
}

}  // namespace

TEST(table_normalises_to_the_12_bit_total) {
  uint32_t counts[8] = {100, 0, 50, 1, 0, 0, 25, 3};
  RansTable t;
  t.build(counts, 8);
  uint32_t sum = 0;
  for (uint32_t s = 0; s < 8; ++s) {
    sum += t.freq(s);
    if (counts[s] == 0) CHECK_EQ(t.freq(s), uint32_t{0});
    else CHECK(t.freq(s) >= 1);
  }
  CHECK_EQ(sum, kRansTotal);
  // Every slot maps to a symbol that owns it.
  for (uint32_t slot = 0; slot < kRansTotal; ++slot) {
    const uint32_t s = t.symbol_at(slot);
    CHECK(slot >= t.start(s) && slot < t.start(s) + t.freq(s));
  }
}

TEST(table_serialisation_round_trips) {
  uint32_t counts[256] = {};
  for (uint32_t i = 0; i < 256; ++i) counts[i] = (i % 7) + 1;
  RansTable a;
  a.build(counts, 256);

  ByteWriter w;
  a.write(w);
  ByteReader r(w.span());
  RansTable b;
  CHECK_ERR(b.read(r), Err::Ok);
  CHECK(r.at_end());
  for (uint32_t s = 0; s < 256; ++s) {
    CHECK_EQ(b.freq(s), a.freq(s));
    CHECK_EQ(b.start(s), a.start(s));
  }
}

TEST(table_rejects_bad_frequencies) {
  // Frequencies that do not fill the 12-bit space would desynchronise the
  // decoder, so they must be refused rather than decoded into garbage.
  ByteWriter w;
  w.varint(2);
  w.varint(0);
  w.varint(100);
  w.varint(1);
  w.varint(100);
  ByteReader r(w.span());
  RansTable t;
  CHECK_ERR(t.read(r), Err::BadMode);

  ByteWriter w2;
  w2.varint(1);
  w2.varint(0);
  w2.varint(0);  // zero frequency
  ByteReader r2(w2.span());
  CHECK_ERR(t.read(r2), Err::BadMode);
}

TEST(coder_round_trips_random_symbols) {
  // Several tables with very different distributions, interleaved at random.
  for (uint64_t seed : {1ull, 2ull, 3ull}) {
    chce_test::Rng rng(seed);
    const uint32_t nctx = 5;
    const uint32_t alphabet = 16;
    std::vector<uint32_t> counts(nctx * alphabet, 0);
    for (uint32_t c = 0; c < nctx; ++c) {
      for (uint32_t s = 0; s < alphabet; ++s) {
        // Context 0 is nearly uniform, context 4 is nearly deterministic.
        const uint32_t weight = c == 4 ? (s == 3 ? 500 : 1) : (1 + (s * (c + 1)) % 13);
        counts[c * alphabet + s] = weight;
      }
    }
    std::vector<RansTable> tables(nctx);
    for (uint32_t c = 0; c < nctx; ++c) tables[c].build(&counts[c * alphabet], alphabet);

    const uint32_t nsym = 5000;
    std::vector<uint32_t> ctx_of(nsym), sym_of(nsym);
    for (uint32_t i = 0; i < nsym; ++i) {
      ctx_of[i] = static_cast<uint32_t>(rng.below(nctx));
      // Sample from the context's distribution.
      const uint32_t* cc = &counts[ctx_of[i] * alphabet];
      uint32_t total = 0;
      for (uint32_t s = 0; s < alphabet; ++s) total += cc[s];
      uint32_t pick = static_cast<uint32_t>(rng.below(total));
      uint32_t s = 0;
      while (pick >= cc[s]) {
        pick -= cc[s];
        ++s;
      }
      sym_of[i] = s;
    }

    // rANS is a stack: symbols go in backwards so the decoder reads forwards.
    RansEncoder enc;
    for (uint32_t i = nsym; i-- > 0;) enc.put(tables[ctx_of[i]], sym_of[i]);
    const auto payload = enc.finish();

    RansDecoder dec(payload);
    CHECK(dec.ok());
    for (uint32_t i = 0; i < nsym; ++i) {
      const uint32_t got = dec.get(tables[ctx_of[i]]);
      CHECK_EQ(got, sym_of[i]);
      if (got != sym_of[i]) break;
    }
  }
}

TEST(coder_compresses_skewed_data) {
  // A highly skewed source must come out far below 8 bits per symbol.
  uint32_t counts[4] = {9000, 900, 90, 10};
  RansTable t;
  t.build(counts, 4);
  RansEncoder enc;
  chce_test::Rng rng(7);
  const uint32_t n = 20000;
  std::vector<uint32_t> syms(n);
  for (uint32_t i = 0; i < n; ++i) {
    syms[i] = rng.below(10000) < 9000 ? 0 : (rng.below(1000) < 900 ? 1 : (rng.below(100) < 90 ? 2 : 3));
  }
  for (uint32_t i = n; i-- > 0;) enc.put(t, syms[i]);
  const auto payload = enc.finish();
  CHECK_MSG(payload.size() < n, "no compression: " + std::to_string(payload.size()) +
                                    " bytes for " + std::to_string(n) + " symbols");

  RansDecoder dec(payload);
  for (uint32_t i = 0; i < n; ++i) {
    CHECK_EQ(dec.get(t), syms[i]);
    if (!dec.ok()) break;
  }
}

TEST(slot_mappings_are_consistent) {
  for (uint32_t len = 2; len <= 273; ++len) {
    const uint32_t s = len_slot(len);
    CHECK(s < 32);
    CHECK(len >= len_base(s));
    if (len_extra_bits(s)) CHECK(len - len_base(s) < (1u << len_extra_bits(s)));
  }
  for (uint32_t dist = 1; dist < (1u << 24); dist = dist * 3 + 1) {
    const uint32_t s = dist_slot(dist);
    CHECK(s < 64);
    CHECK(dist >= dist_base(s));
    if (dist_extra_bits(s)) CHECK(dist - dist_base(s) < (1u << dist_extra_bits(s)));
  }
}

TEST(token_coder_round_trips_a_parse) {
  for (uint32_t alphabet : {2u, 8u, 256u}) {
    auto data = make(40000, alphabet, 100 + alphabet);
    StaticCostModel model(data);
    MatchFinder mf(data);
    std::vector<Token> tokens;
    parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

    ByteWriter w;
    encode_tokens_rans(w, data, 0, static_cast<uint32_t>(data.size()), tokens);

    std::vector<uint8_t> out;
    ByteReader r(w.span());
    CHECK_ERR(decode_tokens_rans(r, static_cast<uint32_t>(data.size()), out), Err::Ok);
    CHECK_MSG(out == data, "rANS token round-trip failed at alphabet " +
                               std::to_string(alphabet));
  }
}

TEST(token_coder_beats_raw_literals) {
  // The whole point of Phase 4: on text, coded output must be far smaller than
  // the uncoded token stream.
  auto data = make(100000, 4, 55);
  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

  ByteWriter coded;
  encode_tokens_rans(coded, data, 0, static_cast<uint32_t>(data.size()), tokens);
  CHECK_MSG(coded.size() < data.size() / 2,
            "coded " + std::to_string(coded.size()) + " from " +
                std::to_string(data.size()));
}

TEST(token_coder_rejects_corruption) {
  auto data = make(20000, 6, 9);
  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

  ByteWriter w;
  encode_tokens_rans(w, data, 0, static_cast<uint32_t>(data.size()), tokens);

  // Truncation at every prefix must be an error, never a crash or a hang.
  const auto full = w.buffer();
  for (size_t n = 0; n < full.size(); n += 97) {
    std::vector<uint8_t> out;
    ByteReader r(std::span<const uint8_t>(full.data(), n));
    const Err e = decode_tokens_rans(r, static_cast<uint32_t>(data.size()), out);
    CHECK_MSG(e != Err::Ok || out.size() == data.size(),
              "truncated stream at " + std::to_string(n) + " decoded to " +
                  std::to_string(out.size()) + " bytes");
  }
}

TEST(adaptive_model_round_trips) {
  // The adaptive path must decode to exactly the same bytes, including across
  // the chunk boundaries where the model changes. Sizes are chosen to give one
  // chunk, a few, and many.
  for (uint32_t alphabet : {2u, 8u, 256u}) {
    for (uint32_t n : {1000u, 20000u, 200000u}) {
      auto data = make(n, alphabet, 300 + alphabet + n);
      StaticCostModel model(data);
      MatchFinder mf(data);
      std::vector<Token> tokens;
      parse_block(data, 0, n, mf, model, tokens);

      ByteWriter w;
      encode_tokens_rans_adaptive(w, data, 0, n, tokens);

      std::vector<uint8_t> out;
      ByteReader r(w.span());
      CHECK_ERR(decode_tokens_rans(r, n, out), Err::Ok);
      CHECK_MSG(out == data, "adaptive round-trip failed: alphabet " +
                                 std::to_string(alphabet) + " n " +
                                 std::to_string(n));
    }
  }
}

TEST(adaptive_wins_on_nonstationary_data) {
  // Two halves with very different statistics: a single static table must
  // average them, an adaptive model can follow. This is the property Phase 5
  // exists for, and the reason it is worth a candidate slot.
  chce_test::Rng rng(4242);
  std::vector<uint8_t> data;
  for (int i = 0; i < 300000; ++i) data.push_back(static_cast<uint8_t>('a' + rng.below(2)));
  for (int i = 0; i < 300000; ++i) data.push_back(static_cast<uint8_t>('a' + rng.below(24)));

  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, static_cast<uint32_t>(data.size()), mf, model, tokens);

  ByteWriter s;
  encode_tokens_rans(s, data, 0, static_cast<uint32_t>(data.size()), tokens);
  ByteWriter a;
  encode_tokens_rans_adaptive(a, data, 0, static_cast<uint32_t>(data.size()), tokens);

  CHECK_MSG(a.size() < s.size(), "adaptive " + std::to_string(a.size()) +
                                     " not smaller than static " +
                                     std::to_string(s.size()) +
                                     " on a non-stationary block");

  std::vector<uint8_t> out;
  ByteReader r(a.span());
  CHECK_ERR(decode_tokens_rans(r, static_cast<uint32_t>(data.size()), out), Err::Ok);
  CHECK(out == data);
}

TEST(adaptive_rejects_corruption) {
  auto data = make(60000, 6, 71);
  StaticCostModel model(data);
  MatchFinder mf(data);
  std::vector<Token> tokens;
  parse_block(data, 0, 60000, mf, model, tokens);
  ByteWriter w;
  encode_tokens_rans_adaptive(w, data, 0, 60000, tokens);

  const auto full = w.buffer();
  for (size_t n = 0; n < full.size(); n += 307) {
    std::vector<uint8_t> out;
    ByteReader r(std::span<const uint8_t>(full.data(), n));
    const Err e = decode_tokens_rans(r, 60000, out);
    CHECK_MSG(e != Err::Ok || out.size() == 60000,
              "truncated adaptive stream at " + std::to_string(n) + " decoded");
  }
}

TEST_MAIN()
