// token_coder.cpp — rANS coding of the parse, static and adaptive.
//
// Two models share the block body:
//
//   model 0 (static)   tables measured on the whole block and transmitted.
//   model 1 (adaptive) the symbol stream is cut into chunks; chunk 0 carries
//                      measured tables, and every later chunk is coded with a
//                      table derived from the symbols already decoded. Nothing
//                      after chunk 0 is transmitted: the decoder recomputes the
//                      same table from its own output.
//
// Why chunks and not per-symbol adaptation: rANS is a stack. The decoder pops
// symbols in the reverse of the encoding order, so the encoder's model state at
// symbol i is *not* the decoder's model state at symbol i — a per-symbol
// adaptive rANS is not decodable in principle. Cutting the stream into
// independent rANS chunks restores a forward, mirrorable order at chunk
// granularity: within a chunk the model is fixed, between chunks both sides
// advance it identically. That is the constraint the Phase 5 note in
// docs/BENCHMARKS.md explains at length.
#include "token_coder.hpp"

#include <algorithm>
#include <cstring>

#include "rans.hpp"
#include "range_coder.hpp"

namespace chce {
namespace {

constexpr uint32_t kFlagCtx = 4;
constexpr uint32_t kLitCtx = 16;
constexpr uint32_t kLenCtx = 4;
constexpr uint32_t kDistCtx = 4;

constexpr uint32_t kFlagAlphabet = 2;
constexpr uint32_t kLitAlphabet = 256;
constexpr uint32_t kLenAlphabet = 32;
constexpr uint32_t kDistAlphabet = 64;

// Table ids inside a symbol list.
// Table ids inside a symbol list, DERIVED from the context counts rather than
// written out. They used to be literals (0/4/20/24/28), and widening kLitCtx
// silently overlapped the literal tables with the length and distance ones — a
// class of bug that is invisible until a round-trip fails. Deriving them makes
// that impossible. The values are unchanged today.
constexpr uint16_t kFlag0 = 0;                       // + kFlagCtx
constexpr uint16_t kLit0 = kFlag0 + kFlagCtx;        // + kLitCtx
constexpr uint16_t kLen0 = kLit0 + kLitCtx;          // + kLenCtx
constexpr uint16_t kDist0 = kLen0 + kLenCtx;         // + kDistCtx
constexpr uint16_t kBypass = kDist0 + kDistCtx;      // uniform, never adaptive
constexpr uint32_t kTableCount = kBypass;

// Symbols per adaptive chunk. Large enough that the per-chunk rANS flush (4
// bytes) is noise, small enough that the model tracks a changing block.
constexpr uint32_t kAdaptChunk = 1024;

uint32_t floor_log2(uint32_t v) {
  uint32_t b = 0;
  while (v >>= 1) ++b;
  return b;
}

RansTable make_bypass_table() {
  const uint32_t counts[2] = {1, 1};
  RansTable t;
  t.build(counts, 2);
  return t;
}

struct Tables {
  RansTable flag[kFlagCtx];
  RansTable lit[kLitCtx];
  RansTable len[kLenCtx];
  RansTable dist[kDistCtx];
};

void write_tables(ByteWriter& w, const Tables& t) {
  for (const auto& x : t.flag) x.write(w);
  for (const auto& x : t.lit) x.write(w);
  for (const auto& x : t.len) x.write(w);
  for (const auto& x : t.dist) x.write(w);
}

Err read_tables(ByteReader& r, Tables& t) {
  for (auto& x : t.flag) {
    if (Err e = x.read(r); e != Err::Ok) return e;
  }
  for (auto& x : t.lit) {
    if (Err e = x.read(r); e != Err::Ok) return e;
  }
  for (auto& x : t.len) {
    if (Err e = x.read(r); e != Err::Ok) return e;
  }
  for (auto& x : t.dist) {
    if (Err e = x.read(r); e != Err::Ok) return e;
  }
  return Err::Ok;
}

uint32_t dist_ctx(uint32_t len_sl) { return std::min(3u, len_sl >> 2); }

// Counts for the adaptive model: one histogram per table.
struct Counts {
  uint32_t c[kTableCount][256] = {};
};

void count_symbols(Counts& m, const std::vector<std::pair<uint16_t, uint8_t>>& syms,
                   size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    const uint16_t id = syms[i].first;
    if (id < kBypass) ++m.c[id][syms[i].second];
  }
}

// Decay by three quarters, never dropping a symbol that has been seen below 1.
// Without the floor a rare symbol decays to zero and becomes uncodable, which
// is a divide-by-zero in RansEncoder::put.
void decay_only(Counts& m) {
  for (uint32_t t = 0; t < kTableCount; ++t) {
    for (uint32_t s = 0; s < 256; ++s) {
      const uint32_t v = (m.c[t][s] * 3) >> 2;
      m.c[t][s] = (m.c[t][s] > 0 && v == 0) ? 1u : v;
    }
  }
}

// Decay by three quarters and fold in the chunk just coded. The effective
// window is a little over one chunk: the model follows the block rather than
// averaging over it.
void decay_and_add(Counts& m, const std::vector<std::pair<uint16_t, uint8_t>>& syms,
                   size_t from, size_t to) {
  for (uint32_t t = 0; t < kTableCount; ++t) {
    for (uint32_t s = 0; s < 256; ++s) m.c[t][s] = (m.c[t][s] * 3) >> 2;
  }
  count_symbols(m, syms, from, to);
}

// Seeds the adaptive model from a table set's normalised frequencies.
//
// Both sides do this from the *same* transmitted tables, so their counts start
// identical — that is what makes the later adaptation reproducible without
// sending anything else. Seeding from the tables rather than from a raw
// histogram matters: the encoder's histogram is on a different scale than the
// 4096-slot frequencies, and the two sides must agree exactly.
void seed_counts_from_tables(Counts& m, const Tables& t) {
  for (uint32_t c = 0; c < kFlagCtx; ++c)
    for (uint32_t s = 0; s < kFlagAlphabet; ++s) m.c[kFlag0 + c][s] = t.flag[c].freq(s);
  for (uint32_t c = 0; c < kLitCtx; ++c)
    for (uint32_t s = 0; s < kLitAlphabet; ++s) m.c[kLit0 + c][s] = t.lit[c].freq(s);
  for (uint32_t c = 0; c < kLenCtx; ++c)
    for (uint32_t s = 0; s < kLenAlphabet; ++s) m.c[kLen0 + c][s] = t.len[c].freq(s);
  for (uint32_t c = 0; c < kDistCtx; ++c)
    for (uint32_t s = 0; s < kDistAlphabet; ++s) m.c[kDist0 + c][s] = t.dist[c].freq(s);
}

void build_tables(const Counts& m, Tables& t) {
  for (uint32_t c = 0; c < kFlagCtx; ++c) t.flag[c].build(m.c[kFlag0 + c], kFlagAlphabet);
  for (uint32_t c = 0; c < kLitCtx; ++c) t.lit[c].build(m.c[kLit0 + c], kLitAlphabet);
  for (uint32_t c = 0; c < kLenCtx; ++c) t.len[c].build(m.c[kLen0 + c], kLenAlphabet);
  for (uint32_t c = 0; c < kDistCtx; ++c) t.dist[c].build(m.c[kDist0 + c], kDistAlphabet);
}

// Turns the token list into the flat symbol sequence, in decode order.
std::vector<std::pair<uint16_t, uint8_t>> build_symbols(
    std::span<const uint8_t> data, uint32_t begin,
    const std::vector<Token>& tokens) {
  std::vector<std::pair<uint16_t, uint8_t>> syms;
  syms.reserve(tokens.size() * 4);

  uint32_t pos = begin;
  for (const Token& t : tokens) {
    const uint32_t ctx = pos & 3;
    if (t.is_literal()) {
      syms.emplace_back(static_cast<uint16_t>(kFlag0 + ctx), 0);
      const uint8_t prev = pos == 0 ? 0 : data[pos - 1];
      syms.emplace_back(static_cast<uint16_t>(kLit0 + (prev >> 4)), data[pos]);
      ++pos;
    } else {
      const uint32_t ls = len_slot(t.len);
      const uint32_t ds = dist_slot(t.dist);

      syms.emplace_back(static_cast<uint16_t>(kFlag0 + ctx), 1);
      syms.emplace_back(static_cast<uint16_t>(kLen0 + ctx), static_cast<uint8_t>(ls));
      uint32_t rem = t.len - len_base(ls);
      for (uint32_t i = len_extra_bits(ls); i-- > 0;) {
        syms.emplace_back(kBypass, static_cast<uint8_t>((rem >> i) & 1));
      }
      syms.emplace_back(static_cast<uint16_t>(kDist0 + dist_ctx(ls)),
                        static_cast<uint8_t>(ds));
      rem = t.dist - dist_base(ds);
      for (uint32_t i = dist_extra_bits(ds); i-- > 0;) {
        syms.emplace_back(kBypass, static_cast<uint8_t>((rem >> i) & 1));
      }
      pos += t.len;
    }
  }
  return syms;
}

const RansTable& table_for(const Tables& t, uint16_t id) {
  if (id < kLit0) return t.flag[id - kFlag0];
  if (id < kLen0) return t.lit[id - kLit0];
  if (id < kDist0) return t.len[id - kLen0];
  return t.dist[id - kDist0];
}

}  // namespace

uint32_t len_slot(uint32_t len) {
  if (len < 10) return len - 2;
  return 8 + floor_log2(len - 9);
}

uint32_t len_base(uint32_t slot) {
  if (slot < 8) return slot + 2;
  return 9 + (1u << (slot - 8));
}

uint32_t len_extra_bits(uint32_t slot) { return slot < 8 ? 0 : slot - 8; }

uint32_t dist_slot(uint32_t dist) {
  if (dist < 4) return dist;
  const uint32_t l = floor_log2(dist);
  return 2 * l + ((dist >> (l - 1)) & 1);
}

uint32_t dist_base(uint32_t slot) {
  if (slot < 4) return slot;
  return (2u | (slot & 1u)) << ((slot >> 1) - 1);
}

uint32_t dist_extra_bits(uint32_t slot) {
  if (slot < 4) return 0;
  return (slot >> 1) - 1;
}

void encode_tokens_rans(ByteWriter& w, std::span<const uint8_t> data,
                        uint32_t begin, uint32_t end,
                        const std::vector<Token>& tokens) {
  (void)end;
  const auto syms = build_symbols(data, begin, tokens);

  // rANS is a stack: emit the symbol sequence backwards so the decoder reads it
  // forwards.
  Counts m;
  count_symbols(m, syms, 0, syms.size());
  Tables tables;
  build_tables(m, tables);

  RansEncoder enc;
  enc.set_single_lane();  // adaptive model: one lane, so counts and state stay in step
  const RansTable bypass = make_bypass_table();
  for (size_t i = syms.size(); i-- > 0;) {
    if (syms[i].first < kBypass) {
      enc.put(table_for(tables, syms[i].first), syms[i].second, i);
    } else {
      enc.put(bypass, syms[i].second, i);
    }
  }
  const std::vector<uint8_t> payload = enc.finish();

  w.u8(0);  // model: static
  write_tables(w, tables);
  w.bytes(payload);
}

void encode_tokens_rans_adaptive(ByteWriter& w, std::span<const uint8_t> data,
                                 uint32_t begin, uint32_t end,
                                 const std::vector<Token>& tokens) {
  (void)end;
  // Chunks are measured in TOKENS, because that is what the decoder counts as
  // it walks the stream. (Slicing by symbol made the decoder expect 3374
  // symbols where there were 1638 tokens, and it ran off the end of the chunk.)
  const size_t total = tokens.size();
  const uint32_t chunk = kAdaptChunk;
  const size_t chunks = total == 0 ? 0 : (total + chunk - 1) / chunk;

  w.u8(1);  // model: adaptive
  w.varint(chunk);
  w.varint(total);

  // Absolute start position of every token, so a chunk can be symbolised on its
  // own without re-walking the block.
  std::vector<uint32_t> tok_pos(total + 1);
  tok_pos[0] = begin;
  for (size_t i = 0; i < total; ++i) tok_pos[i + 1] = tok_pos[i] + tokens[i].len;

  Counts m;
  const RansTable bypass = make_bypass_table();

  // Chunk 0's tables are measured over the WHOLE block, not just chunk 0. That
  // costs nothing extra — one table set is transmitted either way — and it means
  // the adaptive model starts at the static optimum instead of having to learn
  // from scratch. Adaptation can then only improve on it.
  Tables tables0;
  {
    Counts whole;
    for (size_t i = 0; i < total; ++i) {
      const std::vector<Token> one(tokens.begin() + static_cast<long>(i),
                                   tokens.begin() + static_cast<long>(i + 1));
      const auto syms = build_symbols(data, tok_pos[i], one);
      count_symbols(whole, syms, 0, syms.size());
    }
    build_tables(whole, tables0);
    seed_counts_from_tables(m, tables0);
  }

  for (size_t c = 0; c < chunks; ++c) {
    const size_t from = c * chunk;
    const size_t to = std::min(total, from + chunk);
    const std::vector<Token> slice(tokens.begin() + static_cast<long>(from),
                                   tokens.begin() + static_cast<long>(to));
    const auto syms = build_symbols(data, tok_pos[from], slice);

    Tables tables;
    if (c == 0) tables = tables0;
    else build_tables(m, tables);
    if (c == 0) write_tables(w, tables);

    RansEncoder enc;
    for (size_t i = syms.size(); i-- > 0;) {
      if (syms[i].first < kBypass) {
        enc.put(table_for(tables, syms[i].first), syms[i].second, i);
      } else {
        enc.put(bypass, syms[i].second, i);
      }
    }
    const std::vector<uint8_t> payload = enc.finish();
    w.varint(payload.size());
    w.bytes(payload);

    if (c + 1 < chunks) {
      decay_only(m);
      count_symbols(m, syms, 0, syms.size());
    }
  }
}

// Model 2: range coding with per-symbol adaptive bit models.
//
// No tables and no chunks: the model adapts after every bit, which is what the
// Phase 5 spec asked for and what rANS cannot do (it is LIFO). Contexts match
// the rANS path so the two are comparable: flag on position&3, literals on the
// previous byte's high nibble, length slots on position&3, distance slots on
// the length slot.
void encode_tokens_range(ByteWriter& w, std::span<const uint8_t> data,
                         uint32_t begin, uint32_t end,
                         const std::vector<Token>& tokens) {
  (void)end;
  RangeEncoder rc;
  BitModel flag[kFlagCtx];
  std::vector<BitTreeEncoder> lit, len, dist;
  lit.reserve(kLitCtx);
  len.reserve(kLenCtx);
  dist.reserve(kDistCtx);
  for (uint32_t i = 0; i < kLitCtx; ++i) lit.emplace_back(8);
  for (uint32_t i = 0; i < kLenCtx; ++i) len.emplace_back(5);
  for (uint32_t i = 0; i < kDistCtx; ++i) dist.emplace_back(6);

  uint32_t pos = begin;
  for (const Token& t : tokens) {
    const uint32_t ctx = pos & 3;
    if (t.is_literal()) {
      rc.bit(flag[ctx], 0);
      const uint8_t prev = pos == 0 ? 0 : data[pos - 1];
      lit[prev >> 4].encode(rc, data[pos]);
      ++pos;
    } else {
      rc.bit(flag[ctx], 1);
      const uint32_t ls = len_slot(t.len);
      len[ctx].encode(rc, ls);
      uint32_t rem = t.len - len_base(ls);
      for (uint32_t i = len_extra_bits(ls); i-- > 0;) rc.direct((rem >> i) & 1, 1);

      const uint32_t ds = dist_slot(t.dist);
      dist[dist_ctx(ls)].encode(rc, ds);
      rem = t.dist - dist_base(ds);
      for (uint32_t i = dist_extra_bits(ds); i-- > 0;) rc.direct((rem >> i) & 1, 1);
      pos += t.len;
    }
  }

  const std::vector<uint8_t> payload = rc.finish();
  w.u8(2);  // model: range coder
  w.bytes(payload);
}

Err decode_tokens_rans(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out) {
  uint8_t model = 0;
  if (!r.u8(model)) return Err::Truncated;

  const RansTable bypass = make_bypass_table();
  // Every symbol decoded, as (table id, symbol), in decode order. The encoder
  // built its histogram from exactly this sequence; the decoder needs it to
  // advance the model the same way.
  std::vector<std::pair<uint16_t, uint8_t>> recorded;

  // Decodes one chunk's symbols with `tables`, appending to `out`. Returns the
  // number of symbols consumed, or Err on any inconsistency.
  auto decode_chunk = [&](const Tables& tables, size_t want, uint32_t* produced,
                          Err* err) -> size_t {
    std::span<const uint8_t> payload;
    uint64_t len = 0;
    if (!r.varint(len)) {
      *err = Err::BadVarint;
      return 0;
    }
    if (len == 0 || len > r.remaining()) {
      *err = Err::Truncated;
      return 0;
    }
    if (!r.take(static_cast<size_t>(len), payload)) {
      *err = Err::Truncated;
      return 0;
    }

    RansDecoder dec(payload);
    if (!dec.ok()) {
      *err = Err::Corrupt;
      return 0;
    }

    size_t done = 0;
    while (done < want) {
      const uint32_t ctx = static_cast<uint32_t>(out.size()) & 3;
      const uint32_t is_match = dec.get(tables.flag[ctx]);
      if (!dec.ok()) {
        *err = Err::Corrupt;
        return done;
      }

      if (is_match == 0) {
        const uint8_t prev = out.empty() ? 0 : out.back();
        const uint32_t byte = dec.get(tables.lit[prev >> 4]);
        if (!dec.ok()) {
          *err = Err::Corrupt;
          return done;
        }
        // A desynced stream can produce more literals than the block declared;
        // without this check the following match test underflows and asks for
        // an enormous allocation.
        if (*produced >= out_size) {
          *err = Err::Corrupt;
          return done;
        }
        recorded.emplace_back(static_cast<uint16_t>(kFlag0 + ctx), 0);
        recorded.emplace_back(static_cast<uint16_t>(kLit0 + (prev >> 4)),
                              static_cast<uint8_t>(byte));
        out.push_back(static_cast<uint8_t>(byte));
        ++*produced;
        ++done;
        continue;
      }

      const uint32_t ls = dec.get(tables.len[ctx]);
      if (!dec.ok() || ls >= kLenAlphabet) {
        *err = Err::Corrupt;
        return done;
      }
      recorded.emplace_back(static_cast<uint16_t>(kFlag0 + ctx), 1);
      recorded.emplace_back(static_cast<uint16_t>(kLen0 + ctx),
                            static_cast<uint8_t>(ls));
      uint32_t rem = 0;
      for (uint32_t i = 0, n = len_extra_bits(ls); i < n; ++i) {
        rem = (rem << 1) | dec.get(bypass);
        recorded.emplace_back(kBypass, static_cast<uint8_t>(rem & 1));
        if (!dec.ok()) {
          *err = Err::Corrupt;
          return done;
        }
      }
      const uint32_t mlen = len_base(ls) + rem;
      if (mlen < 2 || mlen > 273 || mlen > out_size - *produced) {
        *err = Err::Corrupt;
        return done;
      }

      const uint32_t ds = dec.get(tables.dist[dist_ctx(ls)]);
      if (!dec.ok() || ds >= kDistAlphabet) {
        *err = Err::Corrupt;
        return done;
      }
      recorded.emplace_back(static_cast<uint16_t>(kDist0 + dist_ctx(ls)),
                            static_cast<uint8_t>(ds));
      uint32_t drem = 0;
      for (uint32_t i = 0, n = dist_extra_bits(ds); i < n; ++i) {
        drem = (drem << 1) | dec.get(bypass);
        recorded.emplace_back(kBypass, static_cast<uint8_t>(drem & 1));
        if (!dec.ok()) {
          *err = Err::Corrupt;
          return done;
        }
      }
      const uint32_t dist = dist_base(ds) + drem;
      if (dist == 0 || dist > out.size()) {
        *err = Err::BadMode;
        return done;
      }

      const size_t at = out.size();
      if (dist >= mlen) {
        out.resize(at + mlen);
        std::memcpy(out.data() + at, out.data() + at - dist, mlen);
      } else {
        for (uint32_t k = 0; k < mlen; ++k) out.push_back(out[out.size() - dist]);
      }
      *produced += mlen;
      ++done;
    }
    return done;
  };

  uint32_t produced = 0;

  if (model == 0) {
    Tables tables;
    if (Err e = read_tables(r, tables); e != Err::Ok) return e;
    std::span<const uint8_t> payload;
    if (!r.take(r.remaining(), payload)) return Err::Truncated;
    if (payload.empty()) return Err::Truncated;

    // Re-run the same chunk decoder over the single stream by handing it the
    // payload directly; the static path has no length prefix.
    RansDecoder dec(payload);
    dec.set_single_lane();  // the adaptive model codes everything on lane 0
    if (!dec.ok()) return Err::Corrupt;
    // The static stream is unbounded in symbols, so drive it by output size.
    while (produced < out_size) {
      const uint32_t ctx = static_cast<uint32_t>(out.size()) & 3;
      const uint32_t is_match = dec.get(tables.flag[ctx]);
      if (!dec.ok()) return Err::Corrupt;
      if (is_match == 0) {
        const uint8_t prev = out.empty() ? 0 : out.back();
        const uint32_t byte = dec.get(tables.lit[prev >> 4]);
        if (!dec.ok()) return Err::Corrupt;
        out.push_back(static_cast<uint8_t>(byte));
        ++produced;
        continue;
      }
      const uint32_t ls = dec.get(tables.len[ctx]);
      if (!dec.ok() || ls >= kLenAlphabet) return Err::Corrupt;
      uint32_t rem = 0;
      for (uint32_t i = 0, n = len_extra_bits(ls); i < n; ++i) {
        rem = (rem << 1) | dec.get(bypass);
        if (!dec.ok()) return Err::Corrupt;
      }
      const uint32_t mlen = len_base(ls) + rem;
      if (mlen < 2 || mlen > 273 || mlen > out_size - produced) return Err::Corrupt;
      const uint32_t ds = dec.get(tables.dist[dist_ctx(ls)]);
      if (!dec.ok() || ds >= kDistAlphabet) return Err::Corrupt;
      uint32_t drem = 0;
      for (uint32_t i = 0, n = dist_extra_bits(ds); i < n; ++i) {
        drem = (drem << 1) | dec.get(bypass);
        if (!dec.ok()) return Err::Corrupt;
      }
      const uint32_t dist = dist_base(ds) + drem;
      if (dist == 0 || dist > out.size()) return Err::BadMode;
      const size_t at = out.size();
      if (dist >= mlen) {
        out.resize(at + mlen);
        std::memcpy(out.data() + at, out.data() + at - dist, mlen);
      } else {
        for (uint32_t k = 0; k < mlen; ++k) out.push_back(out[out.size() - dist]);
      }
      produced += mlen;
    }
    return produced == out_size ? Err::Ok : Err::SizeMismatch;
  }

  if (model == 2) {
    std::span<const uint8_t> payload;
    if (!r.take(r.remaining(), payload)) return Err::Truncated;
    if (payload.empty()) return Err::Truncated;

    RangeDecoder rc(payload);
    if (!rc.ok()) return Err::Corrupt;

    BitModel flag[kFlagCtx];
    std::vector<BitTreeDecoder> lit, len, dist;
    lit.reserve(kLitCtx);
    len.reserve(kLenCtx);
    dist.reserve(kDistCtx);
    for (uint32_t i = 0; i < kLitCtx; ++i) lit.emplace_back(8);
    for (uint32_t i = 0; i < kLenCtx; ++i) len.emplace_back(5);
    for (uint32_t i = 0; i < kDistCtx; ++i) dist.emplace_back(6);

    uint32_t produced2 = 0;
    while (produced2 < out_size) {
      const uint32_t ctx = static_cast<uint32_t>(out.size()) & 3;
      if (rc.bit(flag[ctx]) == 0) {
        const uint8_t prev = out.empty() ? 0 : out.back();
        const uint32_t byte = lit[prev >> 4].decode(rc);
        if (!rc.ok()) return Err::Corrupt;
        out.push_back(static_cast<uint8_t>(byte));
        ++produced2;
        continue;
      }
      const uint32_t ls = len[ctx].decode(rc);
      if (!rc.ok() || ls >= kLenAlphabet) return Err::Corrupt;
      uint32_t rem = 0;
      for (uint32_t i = 0, n = len_extra_bits(ls); i < n; ++i) rem = (rem << 1) | rc.direct(1);
      const uint32_t mlen = len_base(ls) + rem;
      if (mlen < 2 || mlen > 273 || mlen > out_size - produced2) return Err::Corrupt;

      const uint32_t ds = dist[dist_ctx(ls)].decode(rc);
      if (!rc.ok() || ds >= kDistAlphabet) return Err::Corrupt;
      uint32_t drem = 0;
      for (uint32_t i = 0, n = dist_extra_bits(ds); i < n; ++i) drem = (drem << 1) | rc.direct(1);
      const uint32_t mdist = dist_base(ds) + drem;
      if (mdist == 0 || mdist > out.size()) return Err::BadMode;

      const size_t at = out.size();
      if (mdist >= mlen) {
        out.resize(at + mlen);
        std::memcpy(out.data() + at, out.data() + at - mdist, mlen);
      } else {
        for (uint32_t k = 0; k < mlen; ++k) out.push_back(out[out.size() - mdist]);
      }
      produced2 += mlen;
    }
    if (!rc.ok()) return Err::Corrupt;
    return produced2 == out_size ? Err::Ok : Err::SizeMismatch;
  }

  if (model != 1) return Err::BadMode;

  uint64_t chunk = 0, total_syms = 0;
  if (!r.varint(chunk)) return Err::BadVarint;
  if (!r.varint(total_syms)) return Err::BadVarint;
  if (chunk == 0 || chunk > (1u << 20)) return Err::BadMode;
  if (total_syms > (1ull << 32)) return Err::BadMode;  // now: total TOKENS

  const size_t chunks =
      total_syms == 0 ? 0 : static_cast<size_t>((total_syms + chunk - 1) / chunk);

  Counts m;
  for (size_t c = 0; c < chunks; ++c) {
    const size_t from = c * static_cast<size_t>(chunk);
    const size_t to =
        std::min<size_t>(static_cast<size_t>(total_syms), from + static_cast<size_t>(chunk));

    Tables tables;
    if (c == 0) {
      if (Err e = read_tables(r, tables); e != Err::Ok) return e;
      seed_counts_from_tables(m, tables);
    } else {
      build_tables(m, tables);
    }

    Err err = Err::Ok;
    const size_t want = to - from;
    const size_t got = decode_chunk(tables, want, &produced, &err);
    if (err != Err::Ok) return err;
    if (got != want) return Err::SizeMismatch;

    // Advance the model exactly as the encoder did: chunk 0 seeds it, later
    // chunks fold in their own symbols after a three-quarter decay.
    if (c + 1 < chunks) {
      decay_only(m);
      for (const auto& [id, sym] : recorded) {
        if (id < kBypass) ++m.c[id][sym];
      }
      recorded.clear();
    }
  }

  return produced == out_size ? Err::Ok : Err::SizeMismatch;
}

}  // namespace chce
