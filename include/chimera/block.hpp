// block.hpp — the block and file headers of CHCE v1, plus their codecs.
//
// Serialisation lives in src/engine/block_writer.cpp. This header only
// declares the shapes so the compressor, decompressor and (later) the mode
// selector share one definition.
#pragma once

#include <cstddef>
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
  // The second Layer 0 byte, added in v5 because the first is full. A separate field
  // rather than a wider integer, so the on-disk shape stays explicit.
  uint8_t layers2 = 0;
  // The third Layer 0 byte, added in v7 because the first two are full. A separate field
  // rather than a wider integer, so the on-disk shape stays explicit.
  uint8_t layers3 = 0;
  // The size AFTER Layer 0 — what the block loop produces, not what the caller passed in.
  uint64_t original_size = 0;
  // The size the caller actually gets, added in v6. Until then the decoder had no way to
  // know the final size, so a crafted stream could describe an enormous expansion and the
  // only bound was a heuristic ceiling checked after the allocation (see
  // docs/AUDIT_FINDINGS.md, B3 and E1). With this field every intermediate buffer is
  // bounded by the true size and the final check is exact rather than a guess.
  uint64_t true_size = 0;
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

// The stream trailer (v8):
//
//   "ZTIX" | block_count varint | sha256(manifest)[32]
//          | block_offset[0..count-1], each u64 little-endian
//          | "ZTOX"
//
// The manifest binds, per block and in order, the mode byte, both sizes and the
// block's own digest — so a dropped, duplicated or reordered block, or an edited
// header field, fails before any payload is decoded.
//
// The offset table is v8's whole addition. Each entry is the absolute position of a
// block's header, counted from the first byte of the stream (the 'C' of CHCE), so an
// SFX stub or a joined split set in front of it changes nothing. Fixed width rather
// than a varint, for one reason: a reader that arrives from the end of the stream can
// compute where the table starts — end - 4 - 8*count — without having parsed a single
// byte inside it. A delta-coded varint table would be about five bytes per block
// smaller and would have to be walked from the front, which is the thing this table
// exists to avoid.
inline constexpr uint8_t kTrailerMagic[4] = {'Z', 'T', 'I', 'X'};
// Marks the end of the trailer. Four bytes that let a reader tell a complete v8 stream
// from one truncated inside its offset table, without trusting any length field.
inline constexpr uint8_t kTrailerTailMagic[4] = {'Z', 'T', 'O', 'X'};
// The fixed cost of the offset table's framing: its four-byte tail marker, and nothing
// else. The entries themselves are 8 bytes each.
inline constexpr size_t kTrailerTailBytes = 4;

// Bytes the trailer occupies for a given block count: "ZTIX", the count varint, the
// manifest, one u64 per block, and the tail marker. Used both by the writer (implicitly)
// and by a reader locating the trailer from the end of the stream.
constexpr size_t trailer_bytes(uint64_t block_count) {
  size_t n = sizeof(kTrailerMagic) + sizeof(kTrailerTailMagic) + 32 + 8 * block_count;
  uint64_t v = block_count;
  do {
    ++n;
    v >>= 7;
  } while (v != 0);
  return n;
}

class BlockManifest {
 public:
  void add(const BlockHeader& h);
  Sha256Digest digest() const;
  size_t entries() const { return serialised_.size(); }

 private:
  std::vector<uint8_t> serialised_;
};

// Writes the trailer. `offsets` holds one absolute stream position per block, in order,
// and must be exactly `block_count` long.
void write_trailer(ByteWriter& w, uint64_t block_count, const Sha256Digest& manifest,
                   std::span<const uint64_t> offsets);

// What the trailer says, once it has been read.
struct TrailerInfo {
  uint64_t block_count = 0;
  Sha256Digest manifest{};
  std::vector<uint64_t> offsets;  // one per block, stream-relative
};

// Sentinel for "the caller has no opinion about how many blocks the trailer should
// describe", used by a reader that is discovering the stream rather than checking it.
inline constexpr uint64_t kAnyBlockCount = ~0ull;

// Reads the trailer at the reader's current position and validates its internal
// structure: the two magics, the table's monotonicity, and that the table is exactly
// `block_count` entries long. `expect_blocks` is checked as soon as the count is read —
// before the table, because a wrong count means the table's length is wrong too, and
// "the trailer disagrees with the block set" is the useful error rather than whatever
// the misaligned parse happens to trip over.
Err read_trailer(ByteReader& r, TrailerInfo& out,
                 uint64_t expect_blocks = kAnyBlockCount);

// Reads the trailer and additionally requires that it agrees with what the caller saw
// while walking the blocks: the count, the manifest digest, and — entry for entry — the
// offsets. A decoder that walked the blocks already knows where they were, so a table
// that disagrees is a lie about the stream and is refused as manifest-mismatch.
Err read_trailer(ByteReader& r, uint64_t expect_blocks, const Sha256Digest& manifest,
                 std::span<const uint64_t> observed_offsets);

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
