// CHCE — Chimera Hybrid Compression Engine
// format.hpp — bitstream v1 primitives: magic, modes, errors, varint codecs.
//
// This header is the single source of truth for the on-disk layout described
// in docs/FORMAT.md. Everything that serialises or parses bytes goes through
// ByteWriter / ByteReader so that bounds checks exist in exactly one place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace chce {

// ---------------------------------------------------------------------------
// Format identity
// ---------------------------------------------------------------------------

inline constexpr uint8_t kMagic[4] = {'C', 'H', 'C', 'E'};
// Version 2 added the trailing CRC-32 of the original bytes.
// v3 added FileHeader::layers (Layer 0 transform bits), because `flags` is full.
// v4 added a SHA-256 per block and a trailer binding the block set, because one
// CRC-32 over the whole original tells you an archive is bad but not which block,
// and only after all the work is done.
// v5 added a second `layers` byte. The first is full: all eight Layer 0 bits are
// allocated and the ninth transform had nowhere to live. Adding a field is a format
// change, hence the bump — the same clean break as v1->v2, v2->v3 and v3->v4.
// v6 added `true_size`, so a crafted stream cannot describe an unbounded expansion.
// v7 added a third `layers` byte, because the first two were full.
// v8 added a block offset table to the trailer: one absolute u64 per block, plus a
// "ZTOX" marker so the table can be found from the end of the stream without walking
// the blocks in front of it. No header field changed, only the trailer, which is why
// the header layout is identical to v7 — and why a v7 stream must still be refused
// rather than read as v8, since its trailer has no table.
// Older streams are rejected with bad-version rather than guessed at.
inline constexpr uint8_t kFormatVersion = 8;

// Default block size. 4 MiB, swept on 2026-11-04 against 64 KiB, 256 KiB, 1 MiB and
// 16 MiB on four corpora (FINDINGS.md §3d). Every backend we have (PPMd, BWT) builds a
// better model from a larger block, and total time is flat; against 256 KiB this is
// 4.3-9.3% smaller on homogeneous content. Memory is the trade: a BWT block needs a few
// multiples of its size.
//
// It is not universally best. On heterogeneous content — 2.75 MB of unrelated source
// files — 1 MiB was 0.6% smaller, because the per-block selector can then choose a
// different backend per block instead of one across all of it. 4 MiB is within 0.6% of
// the best on every corpus measured, so it stays; an adaptive block size is the idea
// that would capture the difference, and is not attempted.
inline constexpr size_t kDefaultBlockSize = 4u * 1024u * 1024u;

// Hard ceiling on a single block's uncompressed size. Guards the decoder
// against absurd varints in fuzzed input (never a real encoder output).
inline constexpr uint64_t kMaxBlockSize = 1ull << 30;  // 1 GiB

// Ceiling on a whole stream's declared original size, for the same reason.
inline constexpr uint64_t kMaxStreamSize = 1ull << 40;  // 1 TiB

// Ceiling on a stream's declared block count. Every block costs at least
// kMinBlockHeaderBytes on disk, so a decoder can always bound this against the
// bytes it actually holds; this constant is the belt to that braces, and it
// keeps a header-only read (a test, a header probe) from accepting a count no
// real stream would carry. 16.7M blocks is 64 GB at 4 KiB blocks and 64 TiB at
// the 4 MiB default.
inline constexpr uint64_t kMaxBlocks = 1ull << 24;

// The smallest a block can possibly be on disk: a mode byte, two varints of at
// least one byte each, and the 32-byte digest. Used to reject a declared block
// count that the stream is physically too short to hold, *before* anything is
// allocated for it.
inline constexpr uint64_t kMinBlockHeaderBytes = 1 + 1 + 1 + 32;

// Header flag bits (see docs/FORMAT.md §Header).
enum Flags : uint8_t {
  kFlagCoarse = 1u << 0,    // coarse (SREP-style) index was consulted
  kFlagMaxRatio = 1u << 1,  // --max-ratio was in effect when written
  kFlagPrecomp = 1u << 2,   // semantic precompression was applied
  kFlagBcj = 1u << 3,       // the x86 BCJ transform was applied to the stream
  // Every block is self-contained, so a decoder may decode them in parallel.
  // Absent means a block may reference data in earlier blocks.
  kFlagParallel = 1u << 4,
  // A coarse dedup stage ran before block splitting.
  kFlagDedup = 1u << 5,
  // A JPEG was recompressed with brunsli before block splitting.
  kFlagJpeg = 1u << 6,
  // A GIF was recompressed from its LZW to palette indices.
  kFlagGif = 1u << 7,
};
// NOTE: all eight bits are now allocated. The next capability needs a new
// header field rather than another bit here.
// Layer 0 transforms, carried in FileHeader::layers. Separate from `flags`
// because that byte is full.
enum LayerFlags : uint8_t {
  kLayerNumeric = 1u << 0,  // integer CSV/TSV columns coded as delta+varint
  // Recursion: the numeric transform was applied to the payload that precomp
  // inflated, i.e. one container level deeper than the input. Undone BEFORE
  // precomp on decode, because it was applied after precomp on encode.
  kLayerNumericNested = 1u << 1,
  // The XML-aware numeric transform: spreadsheet cell values, which is what an
  // .xlsx inflates to. Separate from kLayerNumeric because the payload shape and
  // the inverse are different, and a shared bit would leave the decoder inferring
  // which one it is from kFlagPrecomp.
  kLayerXmlNumeric = 1u << 2,
  kLayerXmlNested = 1u << 3,
  // base64 runs decoded to their bytes. Applied FIRST of all, because every stage
  // after it wants the decoded payload: a JPEG at offset 0 of the payload is what
  // lets brunsli fire, and that was worth 20.2% instead of 2.3% measured.
  kLayerBase64 = 1u << 4,
  // gzip members inflated. A .tar.gz is the most common container there is and
  // precomp reaches ZIP and PDF but not gzip.
  kLayerGzip = 1u << 5,
  // A second container level: DEFLATE recompression applied to what gzip or
  // precomp inflated. A `.tar.gz` holds a tar, and the tar holds a `.xlsx`; the
  // recursion used to stop after re-entering the numeric transforms, so the ZIP
  // inside the tar was never reached.
  kLayerPrecompNested = 1u << 6,
  // 16-bit PCM WAV sample data as per-channel deltas. Bit 7 is the last free one:
  // the next Layer 0 capability needs a second `layers` byte, which is a format
  // change and should be planned as one rather than discovered mid-item.
  kLayerWav = 1u << 7,
};

inline constexpr uint8_t kLayerMask = kLayerNumeric | kLayerNumericNested |
                                      kLayerXmlNumeric | kLayerXmlNested |
                                      kLayerBase64 | kLayerGzip |
                                      kLayerPrecompNested | kLayerWav;

// The second layers byte, added in v5 for the ninth Layer 0 transform onwards.
enum LayerFlags2 : uint8_t {
  // SQLite database pages grouped by B-tree page type. A reordering, not a
  // re-encoding: the decoder recomputes the order from the page type bytes, so the
  // recipe is 12 bytes.
  kLayer2Sqlite = 1u << 0,
  // base64 inside a container: a JSON document holding an embedded blob, reached
  // through the recursion rather than handed over as a bare file.
  kLayer2Base64Nested = 1u << 1,
  // Image transforms one level down. A base64'd JPEG inside a `.json.gz` decodes to
  // a JPEG at offset 0 of the payload, and without these the blob was handed to the
  // backend as ordinary bytes — measured at 0.7% where the bare document gets 19.5%.
  kLayer2JpegNested = 1u << 2,
  kLayer2GifNested = 1u << 3,
  // PNG image data inflated through preflate: a PNG is a DEFLATE stream in a chunk
  // list, and precomp only ever reached ZIP and PDF.
  kLayer2Png = 1u << 4,
  // CD sector parity regenerated rather than stored: 276 bytes a sector of Reed-Solomon
  // that is fully determined by the 2,064 before it. Measured at 30.8% of the compressed
  // size on a synthetic image; re-measured on a real one in ECM_IMPLEMENTATION.md.
  kLayer2Ecc = 1u << 5,
  // BMP pixels split into colour planes and delta-coded per row. Gated by a trial: a
  // flat, tiled sprite sheet is already trivial to compress and the split makes it
  // worse, while a photographic image gains 98.9%.
  kLayer2Bmp = 1u << 6,
  // PNG one level down: a PNG inside a ZIP is inflated by precomp, and until this bit
  // existed the PNG's own DEFLATE was left alone. Measured at -79.9% on a ZIP of six
  // PNGs, because the archive then stores pixels instead of a second compression layer.
  // This is the last free bit in layers2.
  kLayer2PngNested = 1u << 7,
};

// The third layer byte, added in v7 because the first two are full. Bits 1-7 are free.
enum LayerFlags3 : uint8_t {
  // BMP one level down: a ZIP of BMPs stores raw pixels inside a DEFLATE stream inside
  // another DEFLATE stream, and the colour-plane split takes the archive from 120,878 to
  // 1,206 bytes on the fixture that motivated it (-99.0%).
  kLayer3BmpNested = 1u << 0,
  // An .xz stream inflated, with the stream rebuilt from a recipe read out of its own block
  // header: dictionary size, integrity type, and whether the encoder was in extreme mode.
  // Measured headroom is 16.9% on a .tar.xz, and the recipe is proven against a different
  // liblzma version in tests/benchmark/xz_probe.cpp and FINDINGS.md §3h.
  kLayer3Xz = 1u << 1,
  // A brotli stream inflated, with the producing quality, mode and window recovered by trial and
  // confirmed by re-encoding. Measured headroom 14.7% on a .tar.br; brotli has no magic number, so
  // the only test is that the stream decodes and rebuilds byte for byte.
  kLayer3Brotli = 1u << 2,
  // A zstd stream inflated, with the producing level and framing recovered by trial and confirmed
  // by re-encoding. Framing matters: a stream written to a pipe omits its content size, so no
  // seekable encode reproduces it. Measured headroom 22.7% on a .tar.zst, the largest of the three.
  kLayer3Zstd = 1u << 3,
};

inline constexpr uint8_t kLayerMask3 =
    kLayer3BmpNested | kLayer3Xz | kLayer3Brotli | kLayer3Zstd;

inline constexpr uint8_t kLayerMask2 =
    kLayer2Sqlite | kLayer2Base64Nested | kLayer2JpegNested | kLayer2GifNested |
    kLayer2Png | kLayer2Ecc | kLayer2Bmp | kLayer2PngNested;

inline constexpr uint8_t kFlagMask =
    kFlagCoarse | kFlagMaxRatio | kFlagPrecomp | kFlagBcj | kFlagParallel | kFlagDedup | kFlagJpeg | kFlagGif;

// ---------------------------------------------------------------------------
// Block modes
// ---------------------------------------------------------------------------

enum class Mode : uint8_t {
  Raw = 0x00,            // stored, no entropy coding
  RansCtx = 0x01,        // context-modelled rANS (primary)
  HuffStatic = 0x02,     // two-pass Huffman, tree in header
  HuffAdapt = 0x03,      // ARC-style adaptive Huffman
  BwtBsc = 0x04,         // libbsc backend
  Lzma2Fallback = 0x05,  // xz-compatible LZMA2 (Tier-1 safety)
  Brotli = 0x06,
  Zstd = 0x07,
  Lz4 = 0x08,
  CmZpaq = 0x09,  // libzpaq, only with --max-ratio
  Ppmd = 0x0A,
  // Phase 3 transitional mode: an LZ token stream with 8-bit literals and no
  // entropy coding. It exists so the optimal parse can be round-tripped and
  // measured before Phase 4 gives it a real backend; RANS_CTX supersedes it.
  LzRaw = 0x0B,
  // A backend loaded from a plugin. The body names the plugin so the decoder
  // knows which one produced it; the plugin must be present to decode.
  Plugin = 0x0C,
};

constexpr uint8_t mode_byte(Mode m) { return static_cast<uint8_t>(m); }

constexpr bool mode_is_known(uint8_t b) { return b <= mode_byte(Mode::Plugin); }

constexpr const char* mode_name(Mode m) {
  switch (m) {
    case Mode::Raw: return "RAW";
    case Mode::RansCtx: return "RANS_CTX";
    case Mode::HuffStatic: return "HUFF_STATIC";
    case Mode::HuffAdapt: return "HUFF_ADAPT";
    case Mode::BwtBsc: return "BWT_BSC";
    case Mode::Lzma2Fallback: return "LZMA2_FALLBACK";
    case Mode::Brotli: return "BROTLI";
    case Mode::Zstd: return "ZSTD";
    case Mode::Lz4: return "LZ4";
    case Mode::CmZpaq: return "CM_ZPAQ";
    case Mode::Ppmd: return "PPMD";
    case Mode::LzRaw: return "LZ_RAW";
    case Mode::Plugin: return "PLUGIN";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
//
// Every parse path returns one of these; nothing throws, nothing aborts.
// The CLI maps them to a message and a non-zero exit code.
enum class Err : int {
  Ok = 0,
  BadMagic,       // first four bytes are not "CHCE"
  BadVersion,     // version byte is not kFormatVersion
  BadFlags,       // flag byte has bits outside kFlagMask
  // A Layer 0 bit this build does not know. Distinct from BadFlags because the cause is
  // different and so is the fix: the stream was written by a build with a transform this one
  // has never heard of, and the only thing wrong with it is that. Reading it as "invalid
  // header flags" — which is what happened until 2026-11-07 — sends the reader looking for
  // corruption in a file that is perfectly intact.
  UnknownTransform,
  ChecksumMismatch,  // the rebuilt bytes do not match the stored CRC-32
  BadMode,        // mode byte is outside the defined set, or unsupported here
  BadVarint,      // varint is truncated or overflows 64 bits
  Truncated,      // stream ended before a declared field/block completed
  BadBlockSize,   // zero, or above kMaxBlockSize / kMaxStreamSize
  SizeMismatch,   // declared sizes disagree with the bytes actually produced
  TrailingData,   // bytes left over after the last block
  Corrupt,        // an entropy backend rejected its own payload
  BlockChecksumMismatch,  // a block's SHA-256 does not match its bytes
  ManifestMismatch,       // the trailer disagrees with the block set
  // The stream is intact but its blocks are not independently decodable: an
  // archive-mode block may reference bytes an earlier block produced, so asking
  // for one block on its own has no answer. A distinct error rather than
  // `corrupt`, because nothing is corrupt — the request was wrong.
  NotSelfContained,
  Io,             // read or write failure on the CLI side
};

constexpr const char* err_name(Err e) {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::BadMagic: return "bad-magic";
    case Err::BadVersion: return "bad-version";
    case Err::BadFlags: return "bad-flags";
    case Err::UnknownTransform: return "unknown-transform";
    case Err::ChecksumMismatch: return "checksum-mismatch";
    case Err::BadMode: return "bad-mode";
    case Err::BadVarint: return "bad-varint";
    case Err::Truncated: return "truncated";
    case Err::BadBlockSize: return "bad-block-size";
    case Err::SizeMismatch: return "size-mismatch";
    case Err::TrailingData: return "trailing-data";
    case Err::Corrupt: return "corrupt";
    case Err::BlockChecksumMismatch: return "block-checksum-mismatch";
    case Err::ManifestMismatch: return "manifest-mismatch";
    case Err::NotSelfContained: return "not-self-contained";
    case Err::Io: return "io-error";
  }
  return "unknown";
}

// Human-readable one-liner for the CLI.
constexpr const char* err_message(Err e) {
  switch (e) {
    case Err::Ok: return "success";
    case Err::BadMagic: return "not a CHCE stream (bad magic)";
    case Err::BadVersion: return "unsupported CHCE format version";
    case Err::BadFlags: return "invalid header flags";
    case Err::UnknownTransform:
      return "the stream uses a transform this build does not know (written by a newer build?)";
    case Err::ChecksumMismatch: return "checksum mismatch";
    case Err::BadMode: return "invalid or unsupported block mode";
    case Err::BadVarint: return "malformed varint";
    case Err::Truncated: return "stream truncated";
    case Err::BadBlockSize: return "invalid block size";
    case Err::SizeMismatch: return "declared size does not match payload";
    case Err::TrailingData: return "trailing data after final block";
    case Err::Corrupt: return "compressed payload is corrupt";
    case Err::BlockChecksumMismatch: return "a block failed its SHA-256 check";
    case Err::ManifestMismatch: return "the block manifest does not match the stream";
    case Err::NotSelfContained:
      return "blocks are not independently decodable in this stream (archive mode)";
    case Err::Io: return "i/o error";
  }
  return "unknown error";
}

// ---------------------------------------------------------------------------
// ByteWriter — LEB128 varints, bounds-free (grow-only) writer
// ---------------------------------------------------------------------------

class ByteWriter {
 public:
  ByteWriter() = default;

  void u8(uint8_t v) { buf_.push_back(v); }

  // Fixed-width little-endian, for fields whose size must not vary. The offset table
  // uses u64 for exactly this reason: a reader that reaches it from the end of the
  // stream must be able to compute its start without having parsed anything inside it.
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }

  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }

  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf_.push_back(static_cast<uint8_t>(v) | 0x80u);
      v >>= 7;
    }
    buf_.push_back(static_cast<uint8_t>(v));
  }

  void bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    buf_.insert(buf_.end(), b, b + n);
  }

  void bytes(std::span<const uint8_t> s) { bytes(s.data(), s.size()); }

  void reserve(size_t n) { buf_.reserve(n); }
  size_t size() const { return buf_.size(); }
  const std::vector<uint8_t>& buffer() const { return buf_; }
  std::vector<uint8_t> take() { return std::move(buf_); }
  std::span<const uint8_t> span() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// ByteReader — every accessor is bounds-checked and returns bool
// ---------------------------------------------------------------------------

class ByteReader {
 public:
  explicit ByteReader(std::span<const uint8_t> in) : in_(in) {}

  bool u32(uint32_t& v) {
    if (in_.size() - pos_ < 4) return false;
    uint32_t out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(in_[pos_ + i]) << (8 * i);
    pos_ += 4;
    v = out;
    return true;
  }

  bool u64(uint64_t& v) {
    if (in_.size() - pos_ < 8) return false;
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(in_[pos_ + i]) << (8 * i);
    pos_ += 8;
    v = out;
    return true;
  }

  bool u8(uint8_t& v) {
    if (pos_ >= in_.size()) return false;
    v = in_[pos_++];
    return true;
  }

  // LEB128. Rejects truncation and any encoding wider than 64 bits.
  bool varint(uint64_t& v) {
    v = 0;
    unsigned shift = 0;
    while (true) {
      if (pos_ >= in_.size()) return false;  // truncated
      const uint8_t b = in_[pos_++];
      const uint64_t low = b & 0x7Fu;
      if (shift == 63 && low > 1) return false;  // would overflow
      v |= low << shift;
      if ((b & 0x80u) == 0) return true;
      shift += 7;
      if (shift >= 64) return false;  // overlong encoding
    }
  }

  // Reads exactly n bytes as a subspan; false if not enough remain.
  bool take(size_t n, std::span<const uint8_t>& out) {
    if (n > in_.size() - pos_) return false;
    out = in_.subspan(pos_, n);
    pos_ += n;
    return true;
  }

  bool skip(size_t n) {
    if (n > in_.size() - pos_) return false;
    pos_ += n;
    return true;
  }

  size_t remaining() const { return in_.size() - pos_; }
  size_t offset() const { return pos_; }
  bool at_end() const { return pos_ == in_.size(); }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
};

}  // namespace chce
