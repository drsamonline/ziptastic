// block.hpp — the block and file headers of CHCE v1, plus their codecs.
//
// Serialisation lives in src/engine/block_writer.cpp. This header only
// declares the shapes so the compressor, decompressor and (later) the mode
// selector share one definition.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "chimera/format.hpp"

namespace chce {

// The fixed part of a stream header.
struct FileHeader {
  uint8_t version = kFormatVersion;
  uint8_t flags = 0;
  uint64_t original_size = 0;  // exact byte count of the uncompressed input
  uint64_t block_count = 0;    // number of blocks that follow
  // CRC-32 of the original (pre-transform) bytes. Verified after every inverse
  // transform has run, so a decode that silently produced the wrong bytes — the
  // failure mode a transform stack invites — is caught rather than returned.
  uint32_t checksum = 0;

  bool operator==(const FileHeader&) const = default;
};

// The fixed part of a per-block header. Mode-specific metadata (Huffman tree,
// rANS context init, Preflate recipe, …) sits between this and the payload and
// is documented per mode in docs/FORMAT.md.
struct BlockHeader {
  Mode mode = Mode::Raw;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;  // length of the payload that follows

  bool operator==(const BlockHeader&) const = default;
};

// ---------------------------------------------------------------------------
// Serialisation
// ---------------------------------------------------------------------------
//
// Writers append to a ByteWriter and always succeed. Readers are strict: they
// validate every field and return Err::Ok only for a well-formed structure.

// CRC-32 (IEEE 802.3 polynomial, reflected). Table-driven, no dependency.
uint32_t crc32(std::span<const uint8_t> data);

void write_file_header(ByteWriter& w, const FileHeader& h);
Err read_file_header(ByteReader& r, FileHeader& h);

void write_block_header(ByteWriter& w, const BlockHeader& h);
Err read_block_header(ByteReader& r, BlockHeader& h);

}  // namespace chce
