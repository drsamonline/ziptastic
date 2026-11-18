// lz4_wrap.hpp — LZ4 frames: store the plaintext, recover the frame by re-encoding.
//
// An LZ4 frame is a stream of literals and match descriptors. Storing that parse costs more
// than it saves — measured at +49.7% *worse* on a text fixture (docs/LZ4_MEASURED.md), because
// our descriptors are three varints per sequence against LZ4's one or two bytes, and they are
// essentially random.
//
// The way out is not to store the parse at all: LZ4's matcher is deterministic for a given
// parameter set, so the frame can be *recovered* by re-encoding the plaintext and comparing.
// Measured 2026-11-18: 16 of 16 frames recovered by searching a 16-combination grid, and worth
// 36.7% on a text `.lz4` (113,198 → 71,694). A mismatch means decline, which costs nothing.
//
// What is stored: the parameters, the plaintext length, and the plaintext.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool lz4_available();

// Any LZ4 frame in the buffer. Frame magic is 04 22 4D 18, little-endian.
bool lz4_likely(const std::vector<uint8_t>& in);

bool lz4_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);
bool lz4_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
