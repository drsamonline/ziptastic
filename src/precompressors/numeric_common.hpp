// numeric_common.hpp — the varint and canonical-integer primitives shared by the
// Layer 0 numeric transforms (numeric_wrap: delimited text, xml_numeric_wrap:
// spreadsheet XML cell values).
//
// These live in one place because "what counts as a canonical integer" is the
// exactness contract of both transforms. Two copies of that rule would drift, and
// a drift there is silent data loss rather than a compile error.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace chce {
namespace numdetail {

inline void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

inline bool get_varint(std::span<const uint8_t> s, size_t& pos, uint64_t& out) {
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

inline uint64_t zigzag(int64_t x) {
  return (static_cast<uint64_t>(x) << 1) ^ static_cast<uint64_t>(x >> 63);
}
inline int64_t unzigzag(uint64_t u) {
  return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
}

// Strict canonical integer: optional '-', then digits, no leading zero unless the
// value is exactly "0". Returns false for anything that would not re-serialise
// identically, which is what keeps every round trip exact.
inline bool parse_canonical_int(std::span<const uint8_t> s, int64_t& out) {
  if (s.empty() || s.size() > 19) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return false;
  }
  if (s[i] == '0' && s.size() - i > 1) return false;  // leading zero
  int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (s[i] - '0');
  }
  // "-0" parses to 0 and would re-serialise as "0", so it is not canonical. This
  // hole was in the CSV transform too: it accepted "-0" and lost the sign on the
  // way back, and only the compressor's round-trip check kept that from shipping.
  if (neg && v == 0) return false;
  out = neg ? -v : v;
  return true;
}

inline bool parse_canonical_int(const std::string& s, int64_t& out) {
  return parse_canonical_int(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(s.data()), s.size()), out);
}

}  // namespace numdetail
}  // namespace chce
