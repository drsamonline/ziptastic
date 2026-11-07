// zstd_wrap.cpp — zstd inflation with the producing settings recovered by trial.
//
// Same shape as brotli_wrap: decode, try the settings a stream like this plausibly came from, and
// keep the first that re-encodes to exactly the bytes we were given. Nothing of the original is
// stored, so the verifier is the only thing between a wrong guess and wrong data.
#include "zstd_wrap.hpp"

#include <algorithm>
#include <cstring>

#include <zstd.h>

namespace chce {
namespace {

constexpr uint8_t kMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
constexpr uint8_t kPayloadMagic[4] = {'Z', 'S', '0', '1'};
constexpr uint64_t kMaxInflated = 1ull << 32;  // 4 GiB: a transform, not a bomb

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
  // Concatenated frames are legal zstd — `cat a.zst b.zst` — so the streaming decoder is used with
  // the frame-concatenation flag rather than the one-shot call, which stops at the first frame.
  ZSTD_DStream* ds = ZSTD_createDStream();
  if (ds == nullptr) return false;
  ZSTD_initDStream(ds);
  ZSTD_inBuffer ib{in.data(), in.size(), 0};
  uint8_t buf[1 << 16];
  bool ok = false;
  for (;;) {
    ZSTD_outBuffer ob{buf, sizeof(buf), 0};
    const size_t r = ZSTD_decompressStream(ds, &ob, &ib);
    if (ZSTD_isError(r)) break;
    out.insert(out.end(), buf, buf + ob.pos);
    if (out.size() > kMaxInflated) break;
    if (r == 0 && ib.pos == ib.size) {
      ok = true;
      break;
    }
    if (ob.pos == 0 && ib.pos == ib.size && r != 0) break;  // truncated
  }
  ZSTD_freeDStream(ds);
  return ok && !out.empty();
}

bool encode(const std::vector<uint8_t>& raw, int level, bool streaming, std::vector<uint8_t>& out) {
  ZSTD_CCtx* cs = ZSTD_createCCtx();
  if (cs == nullptr) return false;
  bool ok = true;
  // The streaming shape is what a pipe produces: the content size is unknown, so the frame header
  // omits it and the bytes differ from a seekable encode at the same level.
  if (streaming) {
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cs, ZSTD_c_contentSizeFlag, 0))) ok = false;
  }
  if (ok && ZSTD_isError(ZSTD_CCtx_setParameter(cs, ZSTD_c_compressionLevel, level))) ok = false;
  if (ok) {
    out.resize(ZSTD_compressBound(raw.size()));
    size_t n = 0;
    if (streaming) {
      // A pipe does not hand the encoder its whole input at once, and the chunk boundaries decide
      // where zstd flushes blocks — so a one-shot call does not reproduce a piped frame. 128 KiB
      // matches zstd's default block size, which is what a writer with default buffering produces.
      ZSTD_outBuffer ob{out.data(), out.size(), 0};
      constexpr size_t kChunk = 128 * 1024;
      size_t r = 0;
      for (size_t off = 0; off < raw.size(); off += kChunk) {
        ZSTD_inBuffer ib{raw.data() + off, std::min(kChunk, raw.size() - off), 0};
        r = ZSTD_compressStream2(cs, &ob, &ib, ZSTD_e_continue);
        if (ZSTD_isError(r)) break;
      }
      if (!ZSTD_isError(r)) {
        ZSTD_inBuffer empty{raw.data(), 0, 0};
        r = ZSTD_compressStream2(cs, &ob, &empty, ZSTD_e_end);
      }
      n = ZSTD_isError(r) ? r : ob.pos;
    } else {
      n = ZSTD_compress2(cs, out.data(), out.size(), raw.data(), raw.size());
    }
    if (ZSTD_isError(n)) {
      ok = false;
    } else {
      out.resize(n);
    }
  }
  ZSTD_freeCCtx(cs);
  return ok;
}

// The levels worth trying, common ones first. A stream from a level outside this list is declined.
constexpr int kLevels[] = {19, 3, 9, 15, 22, 1, 5};

}  // namespace

bool zstd_available() { return true; }

bool zstd_likely(const std::vector<uint8_t>& in) {
  return in.size() >= sizeof(kMagic) && std::memcmp(in.data(), kMagic, sizeof(kMagic)) == 0;
}

bool zstd_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  if (!zstd_likely(in)) return false;

  std::vector<uint8_t> raw;
  if (!decode(in, raw)) return false;

  int hit_level = 0;
  int hit_framing = -1;
  for (int level : kLevels) {
    for (int framing = 0; framing <= 1 && hit_framing < 0; ++framing) {
      std::vector<uint8_t> again;
      if (!encode(raw, level, framing == 1, again)) continue;
      if (again == in) {
        hit_level = level;
        hit_framing = framing;
      }
    }
    if (hit_framing >= 0) break;
  }
  if (hit_framing < 0) return false;

  payload.clear();
  payload.reserve(raw.size() + 16);
  payload.insert(payload.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
  payload.push_back(static_cast<uint8_t>(hit_level));
  payload.push_back(static_cast<uint8_t>(hit_framing));
  put_varint(payload, raw.size());
  payload.insert(payload.end(), raw.begin(), raw.end());

  std::vector<uint8_t> check;
  if (!zstd_repack(payload, check) || check != in) {
    payload.clear();
    return false;
  }
  return true;
}

bool zstd_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  if (payload.size() < sizeof(kPayloadMagic) + 2 ||
      std::memcmp(payload.data(), kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
    return false;
  }
  size_t pos = sizeof(kPayloadMagic);
  const int level = payload[pos++];
  const int framing = payload[pos++];
  if (level < 1 || level > 22 || framing > 1) return false;
  uint64_t inflated = 0;
  if (!get_varint(payload, pos, inflated)) return false;
  if (inflated != payload.size() - pos) return false;

  const std::vector<uint8_t> raw(payload.begin() + static_cast<long>(pos), payload.end());
  return encode(raw, level, framing == 1, out);
}

}  // namespace chce
