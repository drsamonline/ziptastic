// xz_wrap.hpp — an .xz stream inflated, with the exact bytes reproducible.
//
// `.tar.xz` and `.xz` release assets are opaque to this engine: the block loop is handed a
// compressed stream and can do nothing with it, so the archive comes out slightly larger
// than the input. Measured at 22.7% on a `.tar.zst` and 16.9% on a `.tar.xz` — the headroom
// is real and it is the largest single win available (FINDINGS.md §3h).
//
// The recipe is cheap here in a way it is not for zstd or brotli, and that is why this is the
// first of the three. An .xz block header carries the LZMA2 filter's dictionary size, so the
// producing settings need not be searched for: read the dictionary size, try the two encoder
// shapes (normal and extreme), and one of them reproduces the stream byte for byte.
//
// That was proved before any of this was written, against the vendored liblzma, on streams
// produced by a different liblzma version — `tests/benchmark/xz_probe.cpp`, and the result is
// in FINDINGS.md §3h. What this file adds is the container work: the payload layout, the
// verification, and the refusal to produce anything it has not rebuilt and compared.
#pragma once

#include <cstdint>
#include <vector>

namespace chce {

bool xz_available();

// True when the buffer starts with the .xz magic: FD 37 7A 58 5A 00.
bool xz_likely(const std::vector<uint8_t>& in);

// Payload:
//   "XZ01" | varint dict_size | u8 check | u8 shape | varint inflated_len | inflated bytes
//
// `check` is the .xz integrity type from the stream header (0 none, 1 CRC-32, 4 CRC-64,
// 10 SHA-256); it has to be reproduced because it is part of the bytes. `shape` is 0 for the
// normal encoder and 1 for `extreme`, which is the only other distinction that survives once
// the dictionary size is fixed.
//
// The stream is rebuilt and byte-compared here, on this input, before the payload is
// returned. A recipe that does not reproduce its own stream is refused, so a wrong transform
// costs ratio rather than data.
bool xz_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload);

// Rebuilds the original .xz file from the payload. Returns false rather than producing bytes
// it is not sure about.
bool xz_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

}  // namespace chce
