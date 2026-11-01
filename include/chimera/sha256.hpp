// sha256.hpp — SHA-256, for per-block integrity.
//
// The format carries one CRC-32 over the whole original, checked after every
// inverse transform has run. That tells you the archive is bad; it does not tell
// you which block, and it tells you only after all the work is done. A 32-byte
// digest per block answers both questions before any Layer 0 inverse runs.
//
// Self-contained on purpose: this is the one thing in the format that must not
// depend on a vendored library's licence or API.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace chce {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest sha256(std::span<const uint8_t> data);

inline bool sha256_equal(const Sha256Digest& a, const Sha256Digest& b) {
  // Constant-time comparison: not strictly needed for integrity checking, but it
  // costs nothing and stops anyone reading this from thinking timing mattered.
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

}  // namespace chce
