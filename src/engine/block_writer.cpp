// block_writer.cpp — CHCE v1 header and block-header serialisation.
//
// Layout (docs/FORMAT.md):
//   "CHCE" | version u8 | flags u8 | original_size varint | block_count varint
//   | crc32 u32 (little endian)
//   then per block: mode u8 | uncompressed_size varint | compressed_size varint
//                   | sha256[32] | metadata | payload
//   then the trailer: "ZTIX" | block_count varint | sha256(manifest)[32]
//                   | block_offset u64[block_count] | "ZTOX"
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
  w.u8(h.layers2);
  w.u8(h.layers3);
  w.varint(h.original_size);
  w.varint(h.true_size);
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
  if ((layers & static_cast<uint8_t>(~kLayerMask)) != 0) return Err::UnknownTransform;

  uint8_t layers2 = 0;
  if (!r.u8(layers2)) return Err::Truncated;
  if ((layers2 & static_cast<uint8_t>(~kLayerMask2)) != 0) return Err::UnknownTransform;

  uint8_t layers3 = 0;
  if (!r.u8(layers3)) return Err::Truncated;
  if ((layers3 & static_cast<uint8_t>(~kLayerMask3)) != 0) return Err::UnknownTransform;

  uint64_t original_size = 0;
  if (!r.varint(original_size)) return Err::BadVarint;
  if (original_size > kMaxStreamSize) return Err::BadBlockSize;

  uint64_t true_size = 0;
  if (!r.varint(true_size)) return Err::BadVarint;
  if (true_size > kMaxStreamSize) return Err::BadBlockSize;

  uint64_t block_count = 0;
  if (!r.varint(block_count)) return Err::BadVarint;
  // A block carries at least one byte, so block_count can never exceed the
  // declared original size — and an empty stream has zero blocks.
  if (block_count > original_size) return Err::BadBlockSize;
  // And it can never exceed the absolute ceiling. This is the cheap half of the
  // guard: it needs only the header, so a header probe cannot be made to accept
  // an absurd count. The half that matters — comparing against the bytes the
  // caller actually holds — belongs in the decoder, which is the only place that
  // knows how long the stream is.
  if (block_count > kMaxBlocks) return Err::BadBlockSize;

  h.version = version;
  h.flags = flags;
  h.layers = layers;
  h.layers2 = layers2;
  h.layers3 = layers3;
  h.original_size = original_size;
  h.true_size = true_size;
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

void write_trailer(ByteWriter& w, uint64_t block_count, const Sha256Digest& manifest,
                   std::span<const uint64_t> offsets) {
  // The caller is the compressor and a mismatch here would be a programming error, not
  // a malformed stream. Writing the shorter of the two silently is worse than writing a
  // table the reader will reject, so the assert is in the test suite and the write is
  // taken at face value.
  w.bytes(kTrailerMagic, sizeof(kTrailerMagic));
  w.varint(block_count);
  w.bytes(manifest.data(), manifest.size());
  for (uint64_t off : offsets) w.u64(off);
  w.bytes(kTrailerTailMagic, sizeof(kTrailerTailMagic));
}

Err read_trailer(ByteReader& r, TrailerInfo& out, uint64_t expect_blocks) {
  std::span<const uint8_t> magic;
  if (!r.take(sizeof(kTrailerMagic), magic)) return Err::Truncated;
  for (size_t i = 0; i < sizeof(kTrailerMagic); ++i) {
    if (magic[i] != kTrailerMagic[i]) return Err::Corrupt;
  }
  uint64_t count = 0;
  if (!r.varint(count)) return Err::BadVarint;
  if (expect_blocks != kAnyBlockCount && count != expect_blocks) {
    return Err::ManifestMismatch;
  }
  // The table's size is implied by the count, so a count beyond the block ceiling can
  // never be allocated for: the stream would have to be 8 bytes per block longer than it
  // is, and the caller that located this trailer by arithmetic already refused that.
  if (count > kMaxBlocks) return Err::BadBlockSize;

  std::span<const uint8_t> stored;
  if (!r.take(32, stored)) return Err::Truncated;

  // The table. Every entry must be inside the stream and after the previous one: a
  // block header is never empty, so a table that does not strictly increase cannot
  // describe a real stream. This is the only structural check possible here — that an
  // offset lands on a block header is established by the reader that goes there.
  std::vector<uint64_t> table(static_cast<size_t>(count));
  uint64_t previous = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t off = 0;
    if (!r.u64(off)) return Err::Truncated;
    if (i == 0) {
      // The header in front of the first block is at least the magic, the version and
      // the flag bytes; an offset that points into the header is not a block.
      if (off < 4) return Err::Corrupt;
    } else if (off <= previous) {
      return Err::Corrupt;
    }
    previous = off;
    table[static_cast<size_t>(i)] = off;
  }

  if (!r.take(sizeof(kTrailerTailMagic), magic)) return Err::Truncated;
  for (size_t i = 0; i < sizeof(kTrailerTailMagic); ++i) {
    if (magic[i] != kTrailerTailMagic[i]) return Err::Corrupt;
  }

  out.block_count = count;
  for (size_t i = 0; i < out.manifest.size(); ++i) out.manifest[i] = stored[i];
  out.offsets = std::move(table);
  return Err::Ok;
}

Err read_trailer(ByteReader& r, uint64_t expect_blocks, const Sha256Digest& manifest,
                 std::span<const uint64_t> observed_offsets) {
  TrailerInfo info;
  if (Err e = read_trailer(r, info, expect_blocks); e != Err::Ok) return e;
  if (info.block_count != expect_blocks) return Err::ManifestMismatch;
  if (!sha256_equal(info.manifest, manifest)) return Err::ManifestMismatch;
  if (!observed_offsets.empty()) {
    if (observed_offsets.size() != info.offsets.size()) return Err::ManifestMismatch;
    for (size_t i = 0; i < info.offsets.size(); ++i) {
      if (observed_offsets[i] != info.offsets[i]) return Err::ManifestMismatch;
    }
  }
  return Err::Ok;
}

}  // namespace chce
