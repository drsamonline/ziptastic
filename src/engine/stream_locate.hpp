// stream_locate.hpp — finding where the stream starts inside a buffer.
//
// A self-extracting archive is a launcher stub with the stream appended, so the magic is
// not at offset 0. A joined split set is the same shape. Both decode without a special
// case if the reader looks for the magic instead of assuming it is first.
//
// A false positive costs nothing: the header parse validates magic, version, flags and
// layers, and the CRC-32 of the rebuilt bytes has to match at the end, so a wrong offset
// fails as a bad stream rather than producing wrong data.
#pragma once

#include <algorithm>
#include <cstring>
#include <span>

#include "chimera/format.hpp"

namespace chce {

// The window the scan is willing to walk. A stub bigger than 64 MiB is not a stub.
inline constexpr size_t kLocateLimit = 64u << 20;

inline bool locate_stream(std::span<const uint8_t> in, size_t& at) {
  at = 0;
  if (in.size() < sizeof(kMagic)) return false;
  if (std::memcmp(in.data(), kMagic, sizeof(kMagic)) == 0) return true;

  const size_t limit = std::min<size_t>(in.size(), kLocateLimit);
  size_t pos = 0;
  while (pos + sizeof(kMagic) <= limit) {
    const void* hit = std::memchr(in.data() + pos, kMagic[0], limit - pos);
    if (hit == nullptr) return false;
    pos = static_cast<size_t>(static_cast<const uint8_t*>(hit) - in.data());
    if (pos + sizeof(kMagic) <= in.size() &&
        std::memcmp(in.data() + pos, kMagic, sizeof(kMagic)) == 0) {
      at = pos;
      return true;
    }
    ++pos;
  }
  return false;
}

}  // namespace chce
