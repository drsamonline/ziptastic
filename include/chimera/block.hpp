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
#include "chimera/sha256.hpp"

namespace chce {

// The fixed part of a stream header.
struct FileHeader {
  uint8_t version = kFormatVersion;
  uint8_t flags = 0;
  // Which Layer 0 transforms were applied, as a bitfield. This exists because
  // `flags` above is fully allocated (see format.hpp) and the next capability
  // needed somewhere to live. Adding a field is a format change, hence version 3.
  uint8_t layers = 0;
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
  // SHA-256 of the block's decoded plaintext — the bytes this block contributes
  // before any Layer 0 inverse runs. Checked before those inverses, so a damaged
  // block is named by index instead of surfacing as a whole-archive CRC failure
  // after everything has been decoded.
  Sha256Digest sha256{};

  bool operator==(const BlockHeader&) const = default;
};

// The stream trailer: "ZTIX" | varint block_count | sha256 of the manifest.
// The manifest binds, per block and in order, the mode byte, both sizes and the
// block's own digest — so a dropped, duplicated or reordered block, or an edited
// header field, fails before any payload is decoded.
inline constexpr uint8_t kTrailerMagic[4] = {'Z', 'T', 'I', 'X'};

class BlockManifest {
 public:
  void add(const BlockHeader& h);
  Sha256Digest digest() const;
  size_t entries() const { return serialised_.size(); }

 private:
  std::vector<uint8_t> serialised_;
};

void write_trailer(ByteWriter& w, uint64_t block_count, const Sha256Digest& manifest);
Err read_trailer(ByteReader& r, uint64_t expect_blocks, const Sha256Digest& manifest);

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
