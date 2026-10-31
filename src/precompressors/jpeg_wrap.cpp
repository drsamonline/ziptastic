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

bool jpeg_likely(const std::vector<uint8_t>& in) {
  // SOI (FF D8) followed by any marker (FF xx).
  return in.size() > 4 && in[0] == 0xFF && in[1] == 0xD8 && in[2] == 0xFF;
}

bool jpeg_pack(const std::vector<uint8_t>& in, std::vector<uint8_t>& packed) {
  if (!jpeg_likely(in)) return false;

  std::vector<uint8_t> enc;
  enc.reserve(in.size() / 2);
  if (!EncodeBrunsli(in.size(), in.data(), &enc, collect)) return false;
  if (enc.empty() || enc.size() >= in.size()) return false;  // no gain

  packed = std::move(enc);
  return true;
}

bool jpeg_unpack(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out) {
  std::vector<uint8_t> dec;
  if (!DecodeBrunsli(packed.size(), packed.data(), &dec, collect)) return false;
  if (dec.empty()) return false;
  out = std::move(dec);
  return true;
}

}  // namespace chce
