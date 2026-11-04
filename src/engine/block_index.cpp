// block_index.cpp — reading the v8 block offset table.
//
// The table is at the end of the stream, so it can be found by arithmetic instead of by
// walking the blocks in front of it: a v8 trailer is a fixed size for a given block
// count, and the count is in the header. That is the whole trick, and it is why the
// offsets are fixed-width u64 rather than delta-coded varints.
//
// What this file does not do is trust the table. Every offset is followed to a block
// header, every header is parsed, the headers are re-hashed into the manifest, and the
// blocks must tile the stream with no gap and no overlap. A table that lies fails the
// same check a dropped or reordered block fails, so there is one error for one class of
// problem rather than a new one per way of lying.
#include "chimera/block_index.hpp"

#include <algorithm>

#include "chimera/sha256.hpp"
#include "stream_locate.hpp"

namespace chce {

uint64_t StreamIndex::uncompressed_offset(uint64_t index) const {
  // Linear by design: the caller that wants a position usually wants the block's bytes
  // too, and caching a prefix-sum array would cost more than the add it saves.
  uint64_t sum = 0;
  for (uint64_t i = 0; i < index && i < blocks.size(); ++i) {
    sum += blocks[i].uncompressed_size;
  }
  return sum;
}

bool blocks_are_independently_decodable(const FileHeader& h) {
  // Parallel mode builds a match finder over each block alone, so a block references only
  // itself. Archive mode hands the finder the whole stream and matches reach backwards
  // across block boundaries — which is worth ratio and is exactly what makes a block
  // undecodable on its own.
  return (h.flags & kFlagParallel) != 0;
}

bool stream_is_untransformed(const FileHeader& h) {
  // Stream-level transforms run before block splitting, so their inverse needs the whole
  // stream; the Layer 0 bytes are the same story one level up. A partial decode can run
  // neither, so the block's plaintext is an intermediate rather than the caller's bytes.
  constexpr uint8_t kStreamTransforms =
      kFlagBcj | kFlagDedup | kFlagPrecomp | kFlagJpeg | kFlagGif;
  return h.layers == 0 && h.layers2 == 0 && h.layers3 == 0 &&
         (h.flags & kStreamTransforms) == 0;
}

Err read_index(std::span<const uint8_t> in, StreamIndex& out) {
  out = StreamIndex{};

  size_t at = 0;
  if (!locate_stream(in, at)) return Err::BadMagic;
  out.stream_offset = at;

  ByteReader r(in.subspan(at));
  FileHeader fh;
  if (Err e = read_file_header(r, fh); e != Err::Ok) return e;
  const uint64_t header_bytes = r.offset();
  const uint64_t available = in.size() - at;

  const uint64_t trailer_len = trailer_bytes(fh.block_count);
  if (available < header_bytes + trailer_len) return Err::Truncated;
  const uint64_t trailer_at = available - trailer_len;
  out.trailer_offset = at + trailer_at;

  ByteReader tr(in.subspan(at + trailer_at, static_cast<size_t>(trailer_len)));
  TrailerInfo ti;
  if (Err e = read_trailer(tr, ti); e != Err::Ok) return e;
  if (!tr.at_end()) return Err::Corrupt;
  if (ti.block_count != fh.block_count) return Err::ManifestMismatch;

  out.header = fh;
  out.bytes_read = header_bytes + trailer_len;

  // The first block begins where the file header ends. Anything else is not this format,
  // and the check is free.
  if (fh.block_count > 0 && ti.offsets[0] != header_bytes) return Err::Corrupt;

  BlockManifest manifest;
  uint64_t expected = header_bytes;
  for (uint64_t i = 0; i < fh.block_count; ++i) {
    const uint64_t off = ti.offsets[static_cast<size_t>(i)];
    // Blocks tile the stream: each starts where the previous one's payload ended.
    if (off != expected) return Err::Corrupt;
    if (off >= trailer_at) return Err::Truncated;

    ByteReader br(in.subspan(at + off, static_cast<size_t>(trailer_at - off)));
    BlockHeader bh;
    if (Err e = read_block_header(br, bh); e != Err::Ok) return e;
    const uint64_t body = off + br.offset();
    if (bh.compressed_size > trailer_at - body) return Err::Truncated;

    manifest.add(bh);
    out.blocks.push_back(bh);
    out.block_offsets.push_back(at + off);
    out.body_offsets.push_back(at + body);
    out.bytes_read += br.offset();
    expected = body + bh.compressed_size;
  }
  if (expected != trailer_at) return Err::Corrupt;

  // The digest is the proof that the table found the real blocks and not merely
  // well-formed ones: it covers every header's mode, both sizes and its digest, in order.
  if (!sha256_equal(manifest.digest(), ti.manifest)) return Err::ManifestMismatch;

  // And the declared sizes must add up to what the header promised, which the walking
  // decoder checks as it goes and this one can check without decoding anything.
  uint64_t total = 0;
  for (const BlockHeader& b : out.blocks) total += b.uncompressed_size;
  if (total != fh.original_size) return Err::SizeMismatch;

  return Err::Ok;
}

}  // namespace chce
