// block_writer.cpp — CHCE v1 header and block-header serialisation.
//
// Layout (docs/FORMAT.md):
//   "CHCE" | version u8 | flags u8 | original_size varint | block_count varint
//   | crc32 u32 (little endian)
//   then per block: mode u8 | uncompressed_size varint | compressed_size varint
//                   | sha256[32] | metadata | payload
//   then the trailer: "ZTIX" | block_count varint | sha256(manifest)[32]
#include <array>

#include "chimera/block.hpp"

namespace chce {

uint32_t crc32(std::span<const uint8_t> data) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();

  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : data) c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void write_file_header(ByteWriter& w, const FileHeader& h) {
  w.bytes(kMagic, sizeof(kMagic));
  w.u8(h.version);
  w.u8(h.flags);
  w.u8(h.layers);
  w.varint(h.original_size);
  w.varint(h.block_count);
  w.u32(h.checksum);
}

Err read_file_header(ByteReader& r, FileHeader& h) {
  std::span<const uint8_t> magic;
  if (!r.take(sizeof(kMagic), magic)) return Err::Truncated;
  for (size_t i = 0; i < sizeof(kMagic); ++i) {
    if (magic[i] != kMagic[i]) return Err::BadMagic;
  }

  uint8_t version = 0;
  if (!r.u8(version)) return Err::Truncated;
  if (version != kFormatVersion) return Err::BadVersion;

  uint8_t flags = 0;
  if (!r.u8(flags)) return Err::Truncated;
  if ((flags & static_cast<uint8_t>(~kFlagMask)) != 0) return Err::BadFlags;

  uint8_t layers = 0;
  if (!r.u8(layers)) return Err::Truncated;
  if ((layers & static_cast<uint8_t>(~kLayerMask)) != 0) return Err::BadFlags;

  uint64_t original_size = 0;
  if (!r.varint(original_size)) return Err::BadVarint;
  if (original_size > kMaxStreamSize) return Err::BadBlockSize;

  uint64_t block_count = 0;
  if (!r.varint(block_count)) return Err::BadVarint;
  // A block carries at least one byte, so block_count can never exceed the
  // declared original size — and an empty stream has zero blocks.
  if (block_count > original_size) return Err::BadBlockSize;

  h.version = version;
  h.flags = flags;
  h.layers = layers;
  h.original_size = original_size;
  uint32_t checksum = 0;
  if (!r.u32(checksum)) return Err::Truncated;

  h.block_count = block_count;
  h.checksum = checksum;
  return Err::Ok;
}

void write_block_header(ByteWriter& w, const BlockHeader& h) {
  w.u8(mode_byte(h.mode));
  w.varint(h.uncompressed_size);
  w.varint(h.compressed_size);
  w.bytes(h.sha256.data(), h.sha256.size());
}

Err read_block_header(ByteReader& r, BlockHeader& h) {
  uint8_t mode = 0;
  if (!r.u8(mode)) return Err::Truncated;
  if (!mode_is_known(mode)) return Err::BadMode;

  uint64_t usize = 0;
  if (!r.varint(usize)) return Err::BadVarint;
  if (usize == 0 || usize > kMaxBlockSize) return Err::BadBlockSize;

  uint64_t csize = 0;
  if (!r.varint(csize)) return Err::BadVarint;
  if (csize > r.remaining()) return Err::Truncated;

  std::span<const uint8_t> digest;
  if (!r.take(32, digest)) return Err::Truncated;

  h.mode = static_cast<Mode>(mode);
  h.uncompressed_size = usize;
  h.compressed_size = csize;
  for (size_t i = 0; i < h.sha256.size(); ++i) h.sha256[i] = digest[i];
  return Err::Ok;
}

void BlockManifest::add(const BlockHeader& h) {
  serialised_.push_back(mode_byte(h.mode));
  // Varints by hand so the manifest does not depend on ByteWriter's lifetime.
  for (uint64_t v : {h.uncompressed_size, h.compressed_size}) {
    while (v >= 0x80) {
      serialised_.push_back(static_cast<uint8_t>(v) | 0x80u);
      v >>= 7;
    }
    serialised_.push_back(static_cast<uint8_t>(v));
  }
  serialised_.insert(serialised_.end(), h.sha256.begin(), h.sha256.end());
}

Sha256Digest BlockManifest::digest() const { return sha256(serialised_); }

void write_trailer(ByteWriter& w, uint64_t block_count, const Sha256Digest& manifest) {
  w.bytes(kTrailerMagic, sizeof(kTrailerMagic));
  w.varint(block_count);
  w.bytes(manifest.data(), manifest.size());
}

Err read_trailer(ByteReader& r, uint64_t expect_blocks, const Sha256Digest& manifest) {
  std::span<const uint8_t> magic;
  if (!r.take(sizeof(kTrailerMagic), magic)) return Err::Truncated;
  for (size_t i = 0; i < sizeof(kTrailerMagic); ++i) {
    if (magic[i] != kTrailerMagic[i]) return Err::Corrupt;
  }
  uint64_t count = 0;
  if (!r.varint(count)) return Err::BadVarint;
  if (count != expect_blocks) return Err::ManifestMismatch;

  std::span<const uint8_t> stored;
  if (!r.take(32, stored)) return Err::Truncated;
  for (size_t i = 0; i < manifest.size(); ++i) {
    if (stored[i] != manifest[i]) return Err::ManifestMismatch;
  }
  return Err::Ok;
}

}  // namespace chce
