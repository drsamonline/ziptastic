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

bool jpeg_pack(const std::vector<uint8_t>& in, std::vector<uint8_t>& packed) {
  // Find the first SOI, then try each EOI after it until brunsli accepts one.
  //
  // The first EOI is usually right, but not always: a JPEG with an embedded EXIF
  // thumbnail has two SOI/EOI pairs, and taking the thumbnail's EOI truncates the outer
  // image — brunsli then refuses, and the transform silently does nothing. Trying the
  // candidates costs a few failed encodes and removes the whole class of problem.
  size_t begin = 0;
  bool have_soi = false;
  for (size_t i = 0; i + 3 <= in.size(); ++i) {
    if (in[i] == 0xFF && in[i + 1] == 0xD8 && in[i + 2] == 0xFF) {
      begin = i;
      have_soi = true;
      break;
    }
  }
  if (!have_soi) return false;

  std::vector<uint8_t> enc;
  size_t end = 0;
  bool encoded = false;
  int tries = 0;
  for (size_t j = begin + 3; j + 1 < in.size() && tries < 8; ++j) {
    if (in[j] != 0xFF || in[j + 1] != 0xD9) continue;
    ++tries;
    const size_t candidate = j + 2;
    std::vector<uint8_t> attempt;
    if (!EncodeBrunsli(candidate - begin, in.data() + begin, &attempt, collect)) continue;
    if (attempt.empty() || attempt.size() >= candidate - begin) continue;
    enc = std::move(attempt);
    end = candidate;
    encoded = true;
    break;
  }
  if (!encoded) return false;

  // Payload: varint prefix_len | prefix | varint tail_len | tail | brunsli bytes.
  packed.clear();
  auto put_varint = [&packed](uint64_t x) {
    while (x >= 0x80) {
      packed.push_back(static_cast<uint8_t>(x) | 0x80u);
      x >>= 7;
    }
    packed.push_back(static_cast<uint8_t>(x));
  };
  put_varint(begin);
  packed.insert(packed.end(), in.begin(), in.begin() + static_cast<long>(begin));
  put_varint(in.size() - end);
  packed.insert(packed.end(), in.begin() + static_cast<long>(end), in.end());
  packed.insert(packed.end(), enc.begin(), enc.end());
  return true;
}

bool jpeg_unpack(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  auto get_varint = [&packed, &pos](uint64_t& v) {
    v = 0;
    uint32_t shift = 0;
    while (pos < packed.size()) {
      const uint8_t b = packed[pos++];
      if (shift > 63) return false;
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return true;
      shift += 7;
    }
    return false;
  };

  uint64_t prefix_len = 0, tail_len = 0;
  if (!get_varint(prefix_len)) return false;
  if (prefix_len > packed.size() - pos) return false;
  const size_t prefix_at = pos;
  pos += static_cast<size_t>(prefix_len);
  if (!get_varint(tail_len)) return false;

  // Layout is [prefix_len][prefix][tail_len][tail][brunsli]. The tail sits BEFORE the
  // encoded image, not after it — reading the encoded bytes as "everything except the last
  // tail_len bytes" is the mistake this had, and it only showed up when the tail was
  // non-empty, i.e. in exactly the nested case the scan was added for.
  if (tail_len > packed.size() - pos) return false;
  const size_t tail_at = pos;
  const size_t brunsli_at = pos + static_cast<size_t>(tail_len);

  std::vector<uint8_t> dec;
  const std::vector<uint8_t> enc(packed.begin() + static_cast<long>(brunsli_at),
                                 packed.end());
  if (!DecodeBrunsli(enc.size(), enc.data(), &dec, collect)) return false;
  if (dec.empty()) return false;

  out.clear();
  out.reserve(static_cast<size_t>(prefix_len) + dec.size() + static_cast<size_t>(tail_len));
  out.insert(out.end(), packed.begin() + static_cast<long>(prefix_at),
             packed.begin() + static_cast<long>(prefix_at + prefix_len));
  out.insert(out.end(), dec.begin(), dec.end());
  out.insert(out.end(), packed.begin() + static_cast<long>(tail_at),
             packed.begin() + static_cast<long>(tail_at + tail_len));
  return true;
}

}  // namespace chce
