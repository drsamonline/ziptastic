// token_coder.cpp — two-pass static rANS coding of the parse.
#include "token_coder.hpp"

#include <algorithm>
#include <cstring>

#include "rans.hpp"

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

uint32_t floor_log2(uint32_t v) {
  uint32_t b = 0;
  while (v >>= 1) ++b;
  return b;
}

// A uniform two-symbol table: exactly one bit per extra bit, no distortion.
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
  // Table ids for the symbol list below.
  constexpr uint16_t kFlag0 = 0;    // + ctx
  constexpr uint16_t kLit0 = 4;     // + ctx
  constexpr uint16_t kLen0 = 20;    // + ctx
  constexpr uint16_t kDist0 = 24;   // + ctx
  constexpr uint16_t kBypass = 28;

  // rANS is a stack: the decoder pops symbols in the reverse of the order they
  // were encoded. So the encoder emits the whole symbol sequence backwards,
  // which means the decoder reads it forwards. (An earlier version encoded
  // forwards, which only round-tripped when the input happened to be
  // palindromic — the unit test caught it once the data stopped being kind.)
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
      const uint32_t dc = dist_ctx(ls);

      syms.emplace_back(static_cast<uint16_t>(kFlag0 + ctx), 1);

      syms.emplace_back(static_cast<uint16_t>(kLen0 + ctx), static_cast<uint8_t>(ls));
      uint32_t rem = t.len - len_base(ls);
      for (uint32_t i = len_extra_bits(ls); i-- > 0;) {
        syms.emplace_back(kBypass, static_cast<uint8_t>((rem >> i) & 1));
      }

      syms.emplace_back(static_cast<uint16_t>(kDist0 + dc), static_cast<uint8_t>(ds));
      rem = t.dist - dist_base(ds);
      for (uint32_t i = dist_extra_bits(ds); i-- > 0;) {
        syms.emplace_back(kBypass, static_cast<uint8_t>((rem >> i) & 1));
      }

      pos += t.len;
    }
  }
  (void)end;

  // ---- pass 1: measure ----------------------------------------------------
  uint32_t flag_counts[kFlagCtx][kFlagAlphabet] = {};
  uint32_t lit_counts[kLitCtx][kLitAlphabet] = {};
  uint32_t len_counts[kLenCtx][kLenAlphabet] = {};
  uint32_t dist_counts[kDistCtx][kDistAlphabet] = {};

  for (const auto& [id, sym] : syms) {
    if (id < kLit0) ++flag_counts[id - kFlag0][sym];
    else if (id < kLen0) ++lit_counts[id - kLit0][sym];
    else if (id < kDist0) ++len_counts[id - kLen0][sym];
    else if (id < kBypass) ++dist_counts[id - kDist0][sym];
  }

  Tables tables;
  for (uint32_t c = 0; c < kFlagCtx; ++c) tables.flag[c].build(flag_counts[c], kFlagAlphabet);
  for (uint32_t c = 0; c < kLitCtx; ++c) tables.lit[c].build(lit_counts[c], kLitAlphabet);
  for (uint32_t c = 0; c < kLenCtx; ++c) tables.len[c].build(len_counts[c], kLenAlphabet);
  for (uint32_t c = 0; c < kDistCtx; ++c) tables.dist[c].build(dist_counts[c], kDistAlphabet);
  const RansTable bypass = make_bypass_table();

  // ---- pass 2: encode, last symbol first ----------------------------------
  RansEncoder enc;
  for (size_t i = syms.size(); i-- > 0;) {
    const uint16_t id = syms[i].first;
    const uint32_t sym = syms[i].second;
    if (id < kLit0) enc.put(tables.flag[id - kFlag0], sym);
    else if (id < kLen0) enc.put(tables.lit[id - kLit0], sym);
    else if (id < kDist0) enc.put(tables.len[id - kLen0], sym);
    else if (id < kBypass) enc.put(tables.dist[id - kDist0], sym);
    else enc.put(bypass, sym);
  }

  const std::vector<uint8_t> payload = enc.finish();
  write_tables(w, tables);
  w.bytes(payload);
}

Err decode_tokens_rans(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out) {
  Tables tables;
  if (Err e = read_tables(r, tables); e != Err::Ok) return e;

  std::span<const uint8_t> payload;
  if (!r.take(r.remaining(), payload)) return Err::Truncated;
  if (payload.empty()) return Err::Truncated;

  RansDecoder dec(payload);
  if (!dec.ok()) return Err::Corrupt;

  const RansTable bypass = make_bypass_table();
  uint32_t produced = 0;

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
    const uint32_t len = len_base(ls) + rem;
    if (len < 2 || len > 273) return Err::Corrupt;
    if (len > out_size - produced) return Err::SizeMismatch;

    const uint32_t ds = dec.get(tables.dist[dist_ctx(ls)]);
    if (!dec.ok() || ds >= kDistAlphabet) return Err::Corrupt;
    uint32_t drem = 0;
    for (uint32_t i = 0, n = dist_extra_bits(ds); i < n; ++i) {
      drem = (drem << 1) | dec.get(bypass);
      if (!dec.ok()) return Err::Corrupt;
    }
    const uint32_t dist = dist_base(ds) + drem;
    if (dist == 0 || dist > out.size()) return Err::BadMode;

    // Bulk copy when the match does not overlap its own output; byte-at-a-time
    // only for overlapping runs (which is how runs are coded). push_back per
    // byte was the decode bottleneck.
    const size_t at = out.size();
    if (dist >= len) {
      out.resize(at + len);
      std::memcpy(out.data() + at, out.data() + at - dist, len);
    } else {
      for (uint32_t k = 0; k < len; ++k) out.push_back(out[out.size() - dist]);
    }
    produced += len;
  }

  if (produced != out_size) return Err::SizeMismatch;
  return Err::Ok;
}

}  // namespace chce
