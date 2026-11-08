// xz_wrap.cpp — .xz inflation, with the stream rebuilt and compared before it is accepted.
//
// The interesting part is the recipe: it is four numbers and a shape, because an .xz block
// header states its own dictionary size. Everything else that the encoder decided — match
// finder, nice length, depth, mode — is the same across the presets that share a dictionary
// size, which is what made the probe's re-encodes come out identical (FINDINGS.md §3h).
//
// The decoder re-encodes from the recipe alone. Nothing of the original stream is stored, so
// a recipe that is wrong produces wrong bytes — which is why the encoder rebuilds and compares
// before it returns anything, and why the decoder still ends up covered by the block digest and
// the whole-stream CRC-32.
#include "xz_wrap.hpp"

#include <cstring>

#include <lzma.h>

namespace chce {
namespace {

constexpr uint8_t kMagic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
constexpr uint8_t kPayloadMagic[4] = {'X', 'Z', '0', '1'};

// A ceiling on what an .xz may inflate to. This is a transform, not a bomb: a stream that
// claims more than this is declined, and the block encoder's own bounds still apply after.
// Capped relative to the input, not at a flat 4 GiB — see the note in zstd_wrap.cpp for the
// measurement that prompted this. A stream that inflates past the cap is declined, not allocated.
uint64_t max_inflated(size_t input_size) {
  const uint64_t by_ratio = static_cast<uint64_t>(input_size) * 4096;
  const uint64_t floor_cap = 256ull << 20;
  return by_ratio > floor_cap ? by_ratio : floor_cap;
}

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80u);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(const std::vector<uint8_t>& v, size_t& pos, uint64_t& out) {
  out = 0;
  unsigned shift = 0;
  while (pos < v.size()) {
    const uint8_t b = v[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

bool decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  const uint64_t cap = max_inflated(in.size());
  lzma_stream strm = LZMA_STREAM_INIT;
  // LZMA_CONCATENATED because `cat a.xz b.xz > both.xz` is a valid .xz and a decoder is
  // required to read all of it — the same rule that made gzip a container transform.
  if (lzma_stream_decoder(&strm, cap, LZMA_CONCATENATED) != LZMA_OK) return false;
  strm.next_in = in.data();
  strm.avail_in = in.size();
  uint8_t buf[1 << 16];
  lzma_ret ret = LZMA_OK;
  bool ok = true;
  while (ret == LZMA_OK) {
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    ret = lzma_code(&strm, LZMA_FINISH);
    const size_t got = sizeof(buf) - strm.avail_out;
    out.insert(out.end(), buf, buf + got);
    if (out.size() > cap) {
      ok = false;
      break;
    }
  }
  lzma_end(&strm);
  return ok && ret == LZMA_STREAM_END && !out.empty();
}

// The recipe, read out of the stream's own headers.
struct Recipe {
  uint32_t dict_size = 0;
  uint8_t check = 0;
  // Which preset reproduced the stream, low 7 bits, plus 0x80 for the extreme variant. The
  // dictionary size narrows this to at most two candidates, because the presets' default
  // dictionary sizes collide only in pairs (4 MiB is presets 3 and 4, 8 MiB is 5 and 6) —
  // so this byte costs one trial encode to find, not a search.
  uint8_t preset = 6;
};

bool read_recipe(const std::vector<uint8_t>& in, Recipe& r) {
  lzma_stream_flags flags;
  std::memset(&flags, 0, sizeof(flags));
  if (in.size() < 12 || lzma_stream_header_decode(&flags, in.data()) != LZMA_OK) return false;
  r.check = static_cast<uint8_t>(flags.check);

  // 12-byte stream header, then the first block header. A byte of 0x00 there means the Index,
  // i.e. no blocks — not something this transform has anything to say about.
  const size_t block_at = 12;
  if (in.size() <= block_at || in[block_at] == 0x00) return false;

  lzma_filter filters[LZMA_FILTERS_MAX + 1];
  lzma_block block;
  std::memset(&block, 0, sizeof(block));
  block.version = 0;
  block.check = flags.check;
  block.filters = filters;
  block.header_size = lzma_block_header_size_decode(in[block_at]);
  if (lzma_block_header_decode(&block, nullptr, in.data() + block_at) != LZMA_OK) return false;

  for (int i = 0; filters[i].id != LZMA_VLI_UNKNOWN; ++i) {
    if (filters[i].id != LZMA_FILTER_LZMA2) continue;
    const auto* opt = static_cast<const lzma_options_lzma*>(filters[i].options);
    r.dict_size = opt->dict_size;
    return r.dict_size != 0;
  }
  return false;
}

// Re-encodes from the recipe. This is the whole transform on the decode side: no stored
// original, no diff.
bool encode(const std::vector<uint8_t>& raw, const Recipe& r, std::vector<uint8_t>& out) {
  lzma_options_lzma opt;
  uint32_t preset = r.preset & 0x7Fu;
  if ((r.preset & 0x80u) != 0) preset |= LZMA_PRESET_EXTREME;
  if (lzma_lzma_preset(&opt, preset)) return false;
  // The dictionary size comes from the stream and overrides whatever the preset chose; that is
  // the whole reason the recipe can be three numbers.
  opt.dict_size = r.dict_size;

  lzma_filter filters[2];
  filters[0].id = LZMA_FILTER_LZMA2;
  filters[0].options = &opt;
  filters[1].id = LZMA_VLI_UNKNOWN;
  filters[1].options = nullptr;

  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_stream_encoder(&strm, filters, static_cast<lzma_check>(r.check)) != LZMA_OK) {
    return false;
  }
  strm.next_in = raw.data();
  strm.avail_in = raw.size();
  uint8_t buf[1 << 16];
  lzma_ret ret = LZMA_OK;
  while (ret == LZMA_OK) {
    strm.next_out = buf;
    strm.avail_out = sizeof(buf);
    ret = lzma_code(&strm, LZMA_FINISH);
    const size_t got = sizeof(buf) - strm.avail_out;
    out.insert(out.end(), buf, buf + got);
  }
  lzma_end(&strm);
  return ret == LZMA_STREAM_END;
}

}  // namespace

bool xz_available() { return true; }

bool xz_likely(const std::vector<uint8_t>& in) {
  return in.size() >= sizeof(kMagic) && std::memcmp(in.data(), kMagic, sizeof(kMagic)) == 0;
}

bool xz_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  Recipe r;
  if (!read_recipe(in, r)) return false;

  std::vector<uint8_t> raw;
  if (!decode(in, raw)) return false;

  // Which preset reproduced it. Only the presets whose default dictionary size matches the
  // stream's can possibly be the one, which is at most two of them plus their extreme variants;
  // the order below puts the common ones first so the usual case is one or two trial encodes.
  static constexpr uint8_t kOrder[] = {6, 6 | 0x80, 9, 9 | 0x80, 3, 3 | 0x80, 4, 4 | 0x80,
                                       5, 5 | 0x80, 7, 7 | 0x80, 8, 8 | 0x80, 0, 0 | 0x80,
                                       1, 1 | 0x80, 2, 2 | 0x80};
  bool reproduced = false;
  for (uint8_t candidate : kOrder) {
    lzma_options_lzma opt;
    uint32_t preset = candidate & 0x7Fu;
    if ((candidate & 0x80u) != 0) preset |= LZMA_PRESET_EXTREME;
    if (lzma_lzma_preset(&opt, preset)) continue;
    // A preset that would have chosen a different dictionary size cannot be the producer.
    if (opt.dict_size != r.dict_size) continue;
    Recipe trial = r;
    trial.preset = candidate;
    std::vector<uint8_t> again;
    if (!encode(raw, trial, again)) continue;
    if (again == in) {
      r.preset = candidate;
      reproduced = true;
      break;
    }
  }
  if (!reproduced) return false;

  payload.clear();
  payload.reserve(raw.size() + 16);
  payload.insert(payload.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
  put_varint(payload, r.dict_size);
  payload.push_back(r.check);
  payload.push_back(r.preset);
  put_varint(payload, raw.size());
  payload.insert(payload.end(), raw.begin(), raw.end());

  // And once more from the payload, because that is what the decoder will have: a payload that
  // does not round-trip through its own reader is not a payload.
  std::vector<uint8_t> check;
  if (!xz_repack(payload, check) || check != in) {
    payload.clear();
    return false;
  }
  return true;
}

bool xz_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  if (payload.size() < sizeof(kPayloadMagic) ||
      std::memcmp(payload.data(), kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
    return false;
  }
  size_t pos = sizeof(kPayloadMagic);
  Recipe r;
  uint64_t dict = 0;
  if (!get_varint(payload, pos, dict)) return false;
  if (dict == 0 || dict > (1ull << 30)) return false;
  r.dict_size = static_cast<uint32_t>(dict);
  if (pos + 2 > payload.size()) return false;
  r.check = payload[pos++];
  r.preset = payload[pos++];
  if ((r.preset & 0x7Fu) > 9) return false;
  uint64_t inflated = 0;
  if (!get_varint(payload, pos, inflated)) return false;
  if (inflated != payload.size() - pos) return false;

  const std::vector<uint8_t> raw(payload.begin() + static_cast<long>(pos), payload.end());
  return encode(raw, r, out);
}

}  // namespace chce
