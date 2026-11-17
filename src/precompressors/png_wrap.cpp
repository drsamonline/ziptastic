// png_wrap.cpp — PNG chunk walking around preflate.
//
// The DEFLATE parsing, the correction stream and the re-encode are preflate's. What
// this file adds is the container: which chunks are IDAT, how long each was, and the
// order they go back in. Verified by rebuilding and byte-comparing before the payload
// is returned, like every other transform here.
#include "png_wrap.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <preflate.h>

namespace chce {
namespace {

constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
// Payload version marker. Absent means the original layout, which began with the chunk
// count as a varint; a count whose low seven bits are all set would begin 0xFF, so two of
// them plus a tag cannot be mistaken for one.
constexpr uint8_t kPayloadMagic0 = 0xFF;
constexpr uint8_t kPayloadMagic1 = 0xFF;
constexpr uint8_t kPayloadTag = '2';

uint32_t rd32be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80u);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(const std::vector<uint8_t>& s, size_t& pos, uint64_t& out) {
  out = 0;
  uint32_t shift = 0;
  while (pos < s.size()) {
    const uint8_t b = s[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

// ---- row filters -----------------------------------------------------------
//
// Every PNG row begins with a filter byte and stores deltas against the row above, the
// pixel to the left, or a Paeth prediction of both. That is exactly right for deflate,
// and it is what this engine has always compressed. A context-mixing model does better
// on the *unfiltered* pixels for flat-colour images (a screenshot's rows repeat once the
// deltas are gone) and much worse on photographs, where the deltas are already small.
// Measured both ways in docs/PNG_MEASUREMENT.md: -26.3% on a UI screenshot, +49.7% on a
// photograph. So this is a mode, not a default: the caller measures and picks.

size_t channels_for(uint8_t colour) {
  switch (colour) {
    case 0: return 1;  // greyscale
    case 2: return 3;  // RGB
    case 3: return 1;  // palette index
    case 4: return 2;  // grey + alpha
    case 6: return 4;  // RGBA
    default: return 0;
  }
}

int paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

// Deltas -> true pixels, keeping the filter byte of each row.
bool unfilter_rows(const uint8_t* filtered, size_t n, size_t stride, size_t bpp,
                   std::vector<uint8_t>& pixels, std::vector<uint8_t>& filters) {
  if (stride == 0 || n % (stride + 1) != 0) return false;
  const size_t rows = n / (stride + 1);
  pixels.assign(rows * stride, 0);
  filters.assign(rows, 0);
  for (size_t y = 0; y < rows; ++y) {
    const uint8_t ft = filtered[y * (stride + 1)];
    if (ft > 4) return false;
    filters[y] = ft;
    const uint8_t* src = filtered + y * (stride + 1) + 1;
    uint8_t* out = pixels.data() + y * stride;
    const uint8_t* prev = y ? pixels.data() + (y - 1) * stride : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= bpp ? out[x - bpp] : 0;
      const int b = prev ? prev[x] : 0;
      const int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
      int pred = 0;
      switch (ft) {
        case 0: pred = 0; break;
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) >> 1; break;
        default: pred = paeth(a, b, c); break;
      }
      out[x] = static_cast<uint8_t>(src[x] + pred);
    }
  }
  return true;
}

// The inverse, and the exactness contract: given the pixels and the filter bytes this
// reproduces the original filtered stream byte for byte, which is what preflate's diff
// is applied to on the way back to the original IDAT.
bool refilter_rows(const std::vector<uint8_t>& pixels, const std::vector<uint8_t>& filters,
                   size_t stride, size_t bpp, std::vector<uint8_t>& filtered) {
  const size_t rows = filters.size();
  if (stride == 0 || pixels.size() != rows * stride) return false;
  filtered.assign(rows * (stride + 1), 0);
  for (size_t y = 0; y < rows; ++y) {
    const uint8_t ft = filters[y];
    if (ft > 4) return false;
    filtered[y * (stride + 1)] = ft;
    const uint8_t* src = pixels.data() + y * stride;
    uint8_t* out = filtered.data() + y * (stride + 1) + 1;
    const uint8_t* prev = y ? pixels.data() + (y - 1) * stride : nullptr;
    for (size_t x = 0; x < stride; ++x) {
      const int a = x >= bpp ? src[x - bpp] : 0;
      const int b = prev ? prev[x] : 0;
      const int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
      int pred = 0;
      switch (ft) {
        case 0: pred = 0; break;
        case 1: pred = a; break;
        case 2: pred = b; break;
        case 3: pred = (a + b) >> 1; break;
        default: pred = paeth(a, b, c); break;
      }
      out[x] = static_cast<uint8_t>(src[x] - pred);
    }
  }
  return true;
}

struct Chunk {
  bool idat = false;
  size_t at = 0;    // offset of the chunk (length field) in the input
  size_t len = 0;   // data length
};

// Walks the chunks of ONE PNG starting at `base`, stopping at IEND. Returns the offset just
// past that PNG, or 0 if the bytes there are not a complete PNG.
//
// It used to demand that the walk consume the whole buffer, which is right for a lone file
// and wrong for the payload a ZIP produces: precomp hands over *several* concatenated PNGs
// plus its recipe, and the single-file rule rejected all of them. That is why the nested PNG
// stage measured 0.0% on a ZIP of six PNGs where the ideal was -79.9%.
size_t walk_one(const std::vector<uint8_t>& in, size_t base, std::vector<Chunk>& chunks) {
  if (base + 8 > in.size()) return 0;
  if (std::memcmp(in.data() + base, kSig, 8) != 0) return 0;
  size_t pos = base + 8;
  bool saw_idat = false;
  bool saw_end = false;
  while (pos + 12 <= in.size()) {
    const uint32_t len = rd32be(in.data() + pos);
    if (len > (1u << 30)) return 0;
    if (pos + 12 + len > in.size()) return 0;
    Chunk c;
    c.at = pos;
    c.len = len;
    c.idat = std::memcmp(in.data() + pos + 4, "IDAT", 4) == 0;
    saw_idat = saw_idat || c.idat;
    chunks.push_back(c);
    pos += 12 + len;
    if (std::memcmp(in.data() + c.at + 4, "IEND", 4) == 0) {
      saw_end = true;
      break;
    }
  }
  if (!saw_idat || !saw_end) return 0;
  return pos;
}

}  // namespace

bool png_available() { return true; }

bool png_likely(const std::vector<uint8_t>& in) {
  // Any PNG anywhere, because a container payload puts its own header in front.
  for (size_t i = 0; i + 8 <= in.size(); ++i) {
    if (in[i] != 0x89) continue;
    std::vector<Chunk> chunks;
    if (walk_one(in, i, chunks) != 0) return true;
  }
  return false;
}

// One PNG, at [base, end) of `in`, into its own payload.
bool unpack_one(const std::vector<uint8_t>& in, size_t base, size_t end, bool unfilter,
                std::vector<uint8_t>& payload) {
  std::vector<Chunk> chunks;
  if (walk_one(in, base, chunks) != end) return false;

  std::vector<uint8_t> idat;
  std::vector<uint8_t> skeleton;
  std::vector<uint32_t> idat_lens;
  std::vector<uint32_t> idat_crcs;
  for (const Chunk& c : chunks) {
    if (c.idat) {
      idat.insert(idat.end(), in.begin() + static_cast<long>(c.at + 8),
                  in.begin() + static_cast<long>(c.at + 8 + c.len));
      idat_lens.push_back(static_cast<uint32_t>(c.len));
      // The chunk's CRC is part of the original bytes and is not in the skeleton, so
      // it travels with the length. Computing it would mean shipping a second CRC-32
      // whose polynomial has to match zlib's exactly; four stored bytes cannot be
      // wrong.
      idat_crcs.push_back(rd32be(in.data() + c.at + 8 + c.len));
    } else {
      skeleton.insert(skeleton.end(), in.begin() + static_cast<long>(c.at),
                      in.begin() + static_cast<long>(c.at + 12 + c.len));
    }
  }
  if (idat.empty()) return false;

  // PNG's IDAT stream is zlib-wrapped: a 2-byte header, the raw DEFLATE stream, and a
  // 4-byte Adler-32. preflate parses BARE deflate — a ZIP member has no zlib wrapper —
  // so the wrapper is peeled off here and stored verbatim: six bytes that cannot be
  // mis-reproduced. This is why the first version declined every PNG: it handed
  // preflate the wrapper and preflate rejected the stream.
  if (idat.size() < 6) return false;
  const uint8_t cmf = idat[0];
  const uint8_t flg = idat[1];
  if ((cmf & 0x0F) != 8) return false;   // deflate
  if ((flg & 0x20) != 0) return false;   // FDICT is forbidden in PNG
  if ((static_cast<uint32_t>(cmf) * 256 + flg) % 31 != 0) return false;
  std::vector<unsigned char> raw(idat.begin() + 2, idat.end() - 4);
  const uint8_t wrapper[6] = {idat[0], idat[1], idat[idat.size() - 4],
                              idat[idat.size() - 3], idat[idat.size() - 2],
                              idat[idat.size() - 1]};

  std::vector<unsigned char> inflated;
  std::vector<unsigned char> diff;
  if (!preflate_decode(inflated, diff, raw)) return false;
  // Refuse a stream that inflates past the ceiling. preflate has no cap parameter, so
  // the allocation has already happened by here; what this stops is the expensive part
  // — running the block loop over hundreds of megabytes at 131 KB/s. The ceiling is
  // absolute rather than proportional because deflate cannot exceed ~1032:1, so a ratio
  // cap is unreachable (see png_wrap.hpp).
  if (inflated.size() > kPngMaxInflated) return false;

  // Optionally hand the model true pixels instead of the file's own deltas. Only for
  // 8-bit images, where one filter byte and one byte per channel make the row geometry
  // unambiguous; anything else keeps today's behaviour rather than guessing.
  std::vector<uint8_t> pixels;
  std::vector<uint8_t> filters;
  bool did_unfilter = false;
  if (unfilter) {
    uint32_t w = 0, h = 0;
    uint8_t depth = 0, colour = 0;
    bool have_ihdr = false;
    for (const Chunk& c : chunks) {
      if (std::memcmp(in.data() + c.at + 4, "IHDR", 4) != 0 || c.len < 13) continue;
      const uint8_t* q = in.data() + c.at + 8;
      w = rd32be(q);
      h = rd32be(q + 4);
      depth = q[8];
      colour = q[9];
      have_ihdr = true;
      break;
    }
    const size_t channels = have_ihdr ? channels_for(colour) : 0;
    if (have_ihdr && depth == 8 && channels > 0 && w > 0 && h > 0) {
      const size_t stride = static_cast<size_t>(w) * channels;
      if (inflated.size() == static_cast<size_t>(h) * (stride + 1)) {
        did_unfilter = unfilter_rows(inflated.data(), inflated.size(), stride, channels,
                                     pixels, filters);
      }
    }
  }
  const std::vector<unsigned char>& body = did_unfilter ? pixels : inflated;

  payload.clear();
  payload.reserve(body.size() + filters.size() + diff.size() + skeleton.size() + 64);
  payload.push_back(kPayloadMagic0);
  payload.push_back(kPayloadMagic1);
  payload.push_back(kPayloadTag);
  payload.push_back(did_unfilter ? 1 : 0);
  put_varint(payload, chunks.size());
  for (const Chunk& c : chunks) payload.push_back(c.idat ? 1 : 0);
  for (size_t i = 0; i < idat_lens.size(); ++i) {
    put_varint(payload, idat_lens[i]);
    const uint32_t c = idat_crcs[i];
    payload.push_back(static_cast<uint8_t>(c >> 24));
    payload.push_back(static_cast<uint8_t>(c >> 16));
    payload.push_back(static_cast<uint8_t>(c >> 8));
    payload.push_back(static_cast<uint8_t>(c));
  }
  put_varint(payload, diff.size());
  put_varint(payload, body.size());
  for (int i = 0; i < 6; ++i) payload.push_back(wrapper[i]);
  payload.insert(payload.end(), diff.begin(), diff.end());
  payload.insert(payload.end(), body.begin(), body.end());
  if (did_unfilter) {
    put_varint(payload, filters.size());
    payload.insert(payload.end(), filters.begin(), filters.end());
  }
  payload.insert(payload.end(), skeleton.begin(), skeleton.end());
  return true;
}

// Rebuild the filtered stream from stored pixels + filter bytes. The row geometry is in
// the skeleton (the verbatim non-IDAT chunks), so it is parsed from there rather than
// stored a second time in the payload.
bool refilter_from_skeleton(const std::vector<uint8_t>& payload, size_t skel_at,
                            const std::vector<uint8_t>& pixels,
                            const std::vector<uint8_t>& filters,
                            std::vector<uint8_t>& filtered) {
  // The skeleton is a concatenation of verbatim chunks: 4-byte length, 4-byte type, body,
  // 4-byte CRC. Walk it for IHDR.
  uint32_t w = 0, h = 0;
  uint8_t depth = 0, colour = 0;
  size_t pos = skel_at;
  bool have = false;
  while (pos + 12 <= payload.size()) {
    const uint32_t len = rd32be(payload.data() + pos);
    if (len > (1u << 30) || pos + 12 + len > payload.size()) break;
    if (std::memcmp(payload.data() + pos + 4, "IHDR", 4) == 0 && len >= 13) {
      const uint8_t* q = payload.data() + pos + 8;
      w = rd32be(q);
      h = rd32be(q + 4);
      depth = q[8];
      colour = q[9];
      have = true;
      break;
    }
    pos += 12 + len;
  }
  if (!have || depth != 8) return false;
  const size_t channels = channels_for(colour);
  if (channels == 0 || w == 0 || h == 0) return false;
  if (filters.size() != h) return false;
  return refilter_rows(pixels, filters, static_cast<size_t>(w) * channels, channels, filtered);
}

// One image's payload appended to `out`.
bool repack_one(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  size_t pos = 0;
  bool unfiltered = false;
  if (payload.size() >= 4 && payload[0] == kPayloadMagic0 && payload[1] == kPayloadMagic1 &&
      payload[2] == kPayloadTag) {
    unfiltered = (payload[3] & 1) != 0;
    pos = 4;
  }
  uint64_t nchunks = 0;
  if (!get_varint(payload, pos, nchunks)) return false;
  if (nchunks == 0 || nchunks > (1u << 20)) return false;
  if (pos + nchunks > payload.size()) return false;
  const size_t kinds_at = pos;
  pos += static_cast<size_t>(nchunks);

  std::vector<uint32_t> idat_lens;
  std::vector<uint32_t> idat_crcs;
  for (size_t i = 0; i < nchunks; ++i) {
    if (payload[kinds_at + i] == 1) {
      uint64_t l = 0;
      if (!get_varint(payload, pos, l)) return false;
      if (l > (1u << 30)) return false;
      if (pos + 4 > payload.size()) return false;
      idat_lens.push_back(static_cast<uint32_t>(l));
      idat_crcs.push_back(rd32be(payload.data() + pos));
      pos += 4;
    }
  }
  uint64_t diff_len = 0, infl_len = 0;
  if (!get_varint(payload, pos, diff_len) || !get_varint(payload, pos, infl_len)) {
    return false;
  }
  if (pos + diff_len + infl_len > payload.size()) return false;
  if (pos + 6 > payload.size()) return false;
  const uint8_t wrapper[6] = {payload[pos + 0], payload[pos + 1], payload[pos + 2],
                              payload[pos + 3], payload[pos + 4], payload[pos + 5]};
  pos += 6;

  const size_t diff_at = pos;
  pos += static_cast<size_t>(diff_len);
  const size_t infl_at = pos;
  pos += static_cast<size_t>(infl_len);

  // Unfiltered payloads carry the row filter bytes after the pixel stream; refiltering
  // with them reproduces the original filtered stream exactly, which is what preflate's
  // diff is applied to.
  std::vector<uint8_t> filters;
  if (unfiltered) {
    uint64_t filter_len = 0;
    if (!get_varint(payload, pos, filter_len)) return false;
    if (filter_len == 0 || filter_len > (1u << 22)) return false;
    if (pos + filter_len > payload.size()) return false;
    filters.assign(payload.begin() + static_cast<long>(pos),
                   payload.begin() + static_cast<long>(pos + filter_len));
    pos += static_cast<size_t>(filter_len);
  }
  const size_t skel_at = pos;

  std::vector<unsigned char> raw;
  {
    std::vector<unsigned char> d(payload.begin() + static_cast<long>(diff_at),
                                payload.begin() + static_cast<long>(diff_at + diff_len));
    std::vector<unsigned char> u(payload.begin() + static_cast<long>(infl_at),
                                payload.begin() + static_cast<long>(infl_at + infl_len));
    if (unfiltered) {
      // Pixels back to the file's own deltas, using the filter byte each row had. The
      // row geometry comes from the skeleton's IHDR, which is the only place it survives.
      std::vector<uint8_t> filtered;
      if (!refilter_from_skeleton(payload, skel_at, u, filters, filtered)) return false;
      u = filtered;
    }
    if (!preflate_reencode(raw, d, u)) return false;
  }

  // Rebuild the zlib stream the transform peeled apart.
  std::vector<unsigned char> zstream;
  zstream.reserve(raw.size() + 6);
  zstream.push_back(wrapper[0]);
  zstream.push_back(wrapper[1]);
  zstream.insert(zstream.end(), raw.begin(), raw.end());
  for (int i = 2; i < 6; ++i) zstream.push_back(wrapper[i]);
  raw = zstream;

  out.reserve(out.size() + (payload.size() - skel_at) + raw.size());
  out.insert(out.end(), kSig, kSig + 8);
  size_t skel = 0;
  size_t idat_pos = 0;
  size_t idat_index = 0;
  for (size_t i = 0; i < nchunks; ++i) {
    if (payload[kinds_at + i] == 1) {
      if (idat_index >= idat_lens.size()) return false;
      const size_t l = idat_lens[idat_index++];
      if (idat_pos + l > raw.size()) return false;
      const size_t chunk_at = out.size();
      out.resize(chunk_at + 12 + l);
      out[chunk_at + 0] = static_cast<uint8_t>(l >> 24);
      out[chunk_at + 1] = static_cast<uint8_t>(l >> 16);
      out[chunk_at + 2] = static_cast<uint8_t>(l >> 8);
      out[chunk_at + 3] = static_cast<uint8_t>(l);
      std::memcpy(out.data() + chunk_at + 4, "IDAT", 4);
      std::memcpy(out.data() + chunk_at + 8, raw.data() + idat_pos, l);
      idat_pos += l;
      const uint32_t crc = idat_crcs[idat_index - 1];
      out[chunk_at + 8 + l + 0] = static_cast<uint8_t>(crc >> 24);
      out[chunk_at + 8 + l + 1] = static_cast<uint8_t>(crc >> 16);
      out[chunk_at + 8 + l + 2] = static_cast<uint8_t>(crc >> 8);
      out[chunk_at + 8 + l + 3] = static_cast<uint8_t>(crc);
    } else {
      if (skel_at + skel + 12 > payload.size()) return false;
      const uint32_t l = rd32be(payload.data() + skel_at + skel);
      if (skel_at + skel + 12 + l > payload.size()) return false;
      out.insert(out.end(), payload.begin() + static_cast<long>(skel_at + skel),
                 payload.begin() + static_cast<long>(skel_at + skel + 12 + l));
      skel += 12 + l;
    }
  }
  return skel_at + skel == payload.size() && idat_pos == raw.size();
}

// ---------------------------------------------------------------------------
// The public entry points: a sequence of consecutive PNGs, plus whatever follows them.
//
// A ZIP of PNGs is the case that matters. precomp inflates the members and hands over
// several concatenated PNGs plus its own recipe, and the old single-file rule rejected all
// of it — which is why the nested stage measured 0.0% where the ideal was -79.9%.
//
// Payload: varint image_count | per image: varint payload_len + that image's payload |
// varint tail_len | tail. The tail is whatever came after the last IEND (for a ZIP, the
// precomp recipe), and it is carried verbatim.
// ---------------------------------------------------------------------------

bool png_unpack_mode(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload, bool unfilter);

bool png_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  return png_unpack_mode(in, payload, false);
}

bool png_unpack_unfiltered(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  return png_unpack_mode(in, payload, true);
}

bool png_unpack_mode(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload,
                     bool unfilter) {
  // Scan for the first PNG signature rather than demanding one at offset 0.
  //
  // This is the same move `scan_zip` made for archives inside tar: a container payload puts
  // its own header in front, so a single-file transform that insists on offset 0 never sees
  // the image. preflate's payload is [count][headers][corrections][inflated members]
  // [skeleton], which puts every member image in the middle — measured as a ZIP of six PNGs
  // compressing to 119,051 bytes against an ideal of 23,969 because of exactly this.
  //
  // A false positive costs nothing: walk_one validates every chunk length and requires an
  // IEND, and the stage verifies by rebuilding the input before the payload is adopted.
  size_t first = 0;
  bool found = false;
  while (first + 8 <= in.size()) {
    const void* hit = std::memchr(in.data() + first, 0x89, in.size() - first);
    if (hit == nullptr) break;
    first = static_cast<size_t>(static_cast<const uint8_t*>(hit) - in.data());
    if (first + 8 <= in.size() && std::memcmp(in.data() + first, kSig, 8) == 0) {
      found = true;
      break;
    }
    ++first;
  }
  if (!found) return false;
  const size_t prefix_len = first;

  std::vector<std::pair<size_t, size_t>> segments;
  size_t at = first;
  for (;;) {
    std::vector<Chunk> chunks;
    const size_t end = walk_one(in, at, chunks);
    if (end == 0) break;
    segments.emplace_back(at, end);
    at = end;
    if (segments.size() > (1u << 16)) return false;
  }
  if (segments.empty()) return false;

  std::vector<std::vector<uint8_t>> per_image(segments.size());
  for (size_t i = 0; i < segments.size(); ++i) {
    if (!unpack_one(in, segments[i].first, segments[i].second, unfilter, per_image[i])) return false;
  }

  payload.clear();
  put_varint(payload, prefix_len);
  payload.insert(payload.end(), in.begin(), in.begin() + static_cast<long>(prefix_len));
  put_varint(payload, segments.size());
  for (const auto& img : per_image) {
    put_varint(payload, img.size());
    payload.insert(payload.end(), img.begin(), img.end());
  }
  put_varint(payload, in.size() - at);
  payload.insert(payload.end(), in.begin() + static_cast<long>(at), in.end());
  return true;
}

bool png_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t prefix_len = 0;
  if (!get_varint(payload, pos, prefix_len)) return false;
  if (prefix_len > payload.size() - pos) return false;
  const size_t prefix_at = pos;
  pos += static_cast<size_t>(prefix_len);

  uint64_t count = 0;
  if (!get_varint(payload, pos, count)) return false;
  if (count == 0 || count > (1u << 16)) return false;

  out.assign(payload.begin() + static_cast<long>(prefix_at),
             payload.begin() + static_cast<long>(prefix_at + prefix_len));
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t len = 0;
    if (!get_varint(payload, pos, len)) return false;
    if (len > payload.size() - pos) return false;
    const std::vector<uint8_t> one(payload.begin() + static_cast<long>(pos),
                                   payload.begin() + static_cast<long>(pos + len));
    if (!repack_one(one, out)) return false;
    pos += static_cast<size_t>(len);
  }
  uint64_t tail = 0;
  if (!get_varint(payload, pos, tail)) return false;
  if (tail != payload.size() - pos) return false;
  out.insert(out.end(), payload.begin() + static_cast<long>(pos), payload.end());
  return true;
}

}  // namespace chce
