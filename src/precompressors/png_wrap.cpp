// png_wrap.cpp — PNG chunk walking around preflate.
//
// The DEFLATE parsing, the correction stream and the re-encode are preflate's. What
// this file adds is the container: which chunks are IDAT, how long each was, and the
// order they go back in. Verified by rebuilding and byte-comparing before the payload
// is returned, like every other transform here.
#include "png_wrap.hpp"

#include <cstring>

#include <preflate.h>

namespace chce {
namespace {

constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

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

struct Chunk {
  bool idat = false;
  size_t at = 0;    // offset of the chunk (length field) in the input
  size_t len = 0;   // data length
};

bool walk(const std::vector<uint8_t>& in, std::vector<Chunk>& chunks) {
  if (in.size() < 8 || std::memcmp(in.data(), kSig, 8) != 0) return false;
  size_t pos = 8;
  bool saw_idat = false;
  while (pos + 12 <= in.size()) {
    const uint32_t len = rd32be(in.data() + pos);
    if (len > (1u << 30)) return false;
    if (pos + 12 + len > in.size()) return false;
    Chunk c;
    c.at = pos;
    c.len = len;
    c.idat = std::memcmp(in.data() + pos + 4, "IDAT", 4) == 0;
    saw_idat = saw_idat || c.idat;
    chunks.push_back(c);
    pos += 12 + len;
    if (std::memcmp(in.data() + c.at + 4, "IEND", 4) == 0) break;
  }
  return saw_idat && pos == in.size();
}

}  // namespace

bool png_available() { return true; }

bool png_likely(const std::vector<uint8_t>& in) {
  std::vector<Chunk> chunks;
  return walk(in, chunks);
}

bool png_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  std::vector<Chunk> chunks;
  if (!walk(in, chunks)) return false;

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

  payload.clear();
  payload.reserve(inflated.size() + diff.size() + skeleton.size() + 64);
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
  put_varint(payload, inflated.size());
  for (int i = 0; i < 6; ++i) payload.push_back(wrapper[i]);
  payload.insert(payload.end(), diff.begin(), diff.end());
  payload.insert(payload.end(), inflated.begin(), inflated.end());
  payload.insert(payload.end(), skeleton.begin(), skeleton.end());
  return true;
}

bool png_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  size_t pos = 0;
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
  const size_t skel_at = pos;

  std::vector<unsigned char> raw;
  {
    std::vector<unsigned char> d(payload.begin() + static_cast<long>(diff_at),
                                payload.begin() + static_cast<long>(diff_at + diff_len));
    std::vector<unsigned char> u(payload.begin() + static_cast<long>(infl_at),
                                payload.begin() + static_cast<long>(infl_at + infl_len));
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

  out.clear();
  out.reserve((payload.size() - skel_at) + raw.size());
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

}  // namespace chce
