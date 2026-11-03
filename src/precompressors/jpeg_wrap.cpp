// jpeg_wrap.cpp — thin wrapper around brunsli.
#include "jpeg_wrap.hpp"

#include <cstring>

#include <brunsli/decode.h>
#include <brunsli/encode.h>

namespace chce {
namespace {

// brunsli streams its output through a callback; we collect it into a vector.
size_t collect(void* sink, const uint8_t* buf, size_t n) {
  auto* out = static_cast<std::vector<uint8_t>*>(sink);
  out->insert(out->end(), buf, buf + n);
  return n;
}

}  // namespace

bool jpeg_available() { return true; }

// Any JPEG anywhere in the buffer, not only at offset 0.
//
// A container payload puts its own header in front of the member bytes, so a single-file
// transform that insists on offset 0 never sees an image inside a ZIP. PNG got an
// anywhere-scan and gained 79.2% on a ZIP of PNGs; this is the same move for JPEG.
//
// The extent is found by scanning for EOI (FF D9). Inside entropy-coded data a literal FF is
// always followed by 00 or a marker, so the first FF D9 after the SOI is the real end. A
// wrong guess costs nothing: the stage rebuilds the input and byte-compares before adopting
// the payload, and brunsli refuses anything that is not a JPEG anyway.
// A signature check, cheap and permissive: any SOI marker anywhere. Whether a complete
// image can be delimited is jpeg_pack's problem, not this gate's — a header fragment is
// still "jpeg-ish", and the caller uses this only to decide whether the expensive path is
// worth trying.
bool jpeg_likely(const std::vector<uint8_t>& in) {
  for (size_t i = 0; i + 3 <= in.size(); ++i) {
    if (in[i] == 0xFF && in[i + 1] == 0xD8 && in[i + 2] == 0xFF) return true;
  }
  return false;
}

namespace {

// Encodes the JPEG that starts at `begin`, trying each EOI after it until brunsli accepts
// one. Returns the end offset, or 0.
//
// The first EOI is usually right, but not always: a JPEG with an embedded EXIF thumbnail
// has two SOI/EOI pairs, and taking the thumbnail's EOI truncates the outer image — brunsli
// then refuses and the transform silently does nothing. Trying the candidates costs a few
// failed encodes and removes the whole class of problem.
size_t encode_one(const std::vector<uint8_t>& in, size_t begin, std::vector<uint8_t>& enc) {
  int tries = 0;
  for (size_t j = begin + 3; j + 1 < in.size() && tries < 8; ++j) {
    if (in[j] != 0xFF || in[j + 1] != 0xD9) continue;
    ++tries;
    const size_t candidate = j + 2;
    std::vector<uint8_t> attempt;
    if (!EncodeBrunsli(candidate - begin, in.data() + begin, &attempt, collect)) continue;
    if (attempt.empty() || attempt.size() >= candidate - begin) continue;
    enc = std::move(attempt);
    return candidate;
  }
  return 0;
}

size_t find_soi(const std::vector<uint8_t>& in, size_t from) {
  for (size_t i = from; i + 3 <= in.size(); ++i) {
    if (in[i] == 0xFF && in[i + 1] == 0xD8 && in[i + 2] == 0xFF) return i;
  }
  return 0;
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
  uint32_t shift = 0;
  while (pos < v.size()) {
    const uint8_t b = v[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

}  // namespace

// Every JPEG in the buffer, not just the first.
//
// A ZIP of JPEGs puts several members in one payload, and transforming only the first left
// the rest at their original size: measured at -5.1% where a single-image ZIP gets -15.6%.
// The images are not necessarily adjacent — the container's own bytes sit before the first
// and after the last — so the payload carries a prefix, a count, the encoded images and a
// tail, and the bytes *between* images ride in the tail of the previous one.
bool jpeg_pack(const std::vector<uint8_t>& in, std::vector<uint8_t>& packed) {
  size_t begin = find_soi(in, 0);
  if (begin == 0 && !(in.size() > 2 && in[0] == 0xFF && in[1] == 0xD8)) return false;
  if (in.size() < 3 || in[begin] != 0xFF || in[begin + 1] != 0xD8) return false;

  std::vector<std::vector<uint8_t>> encoded;
  size_t at = begin;
  for (;;) {
    std::vector<uint8_t> enc;
    const size_t end = encode_one(in, at, enc);
    if (end == 0) break;
    encoded.push_back(std::move(enc));
    if (encoded.size() > (1u << 14)) return false;
    const size_t next = find_soi(in, end);
    if (next == 0) {
      at = end;
      break;
    }
    at = next;
  }
  if (encoded.empty()) return false;
  const size_t tail_at = at;

  packed.clear();
  put_varint(packed, begin);
  packed.insert(packed.end(), in.begin(), in.begin() + static_cast<long>(begin));
  put_varint(packed, encoded.size());
  for (const auto& enc : encoded) {
    put_varint(packed, enc.size());
    packed.insert(packed.end(), enc.begin(), enc.end());
  }
  put_varint(packed, in.size() - tail_at);
  packed.insert(packed.end(), in.begin() + static_cast<long>(tail_at), in.end());
  return true;
}

bool jpeg_unpack(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t prefix_len = 0;
  if (!get_varint(packed, pos, prefix_len)) return false;
  if (prefix_len > packed.size() - pos) return false;
  const size_t prefix_at = pos;
  pos += static_cast<size_t>(prefix_len);

  uint64_t count = 0;
  if (!get_varint(packed, pos, count)) return false;
  if (count == 0 || count > (1u << 14)) return false;

  out.assign(packed.begin() + static_cast<long>(prefix_at),
             packed.begin() + static_cast<long>(prefix_at + prefix_len));
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t len = 0;
    if (!get_varint(packed, pos, len)) return false;
    if (len > packed.size() - pos) return false;
    std::vector<uint8_t> dec;
    if (!DecodeBrunsli(static_cast<size_t>(len), packed.data() + pos, &dec, collect)) {
      return false;
    }
    if (dec.empty()) return false;
    out.insert(out.end(), dec.begin(), dec.end());
    pos += static_cast<size_t>(len);
  }

  uint64_t tail_len = 0;
  if (!get_varint(packed, pos, tail_len)) return false;
  if (tail_len != packed.size() - pos) return false;
  out.insert(out.end(), packed.begin() + static_cast<long>(pos), packed.end());
  return true;
}

}  // namespace chce
