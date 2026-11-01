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
inline constexpr uint8_t kFormatVersion = 3;

// Default block size. The prompt starts at 256 KB; Phase 12 tunes it.
// 4 MiB. Measured: on dickens, 256 KiB gives 2,743,444 bytes, 1 MiB 2,496,909,
// 4 MiB 2,355,513 — every backend we have (PPMd, BWT) builds a better model
// from a larger block, and total time is flat. Memory is the trade: a BWT block
// needs a few multiples of its size.
inline constexpr size_t kDefaultBlockSize = 4u * 1024u * 1024u;

// Hard ceiling on a single block's uncompressed size. Guards the decoder
// against absurd varints in fuzzed input (never a real encoder output).
inline constexpr uint64_t kMaxBlockSize = 1ull << 30;  // 1 GiB

// Ceiling on a whole stream's declared original size, for the same reason.
inline constexpr uint64_t kMaxStreamSize = 1ull << 40;  // 1 TiB

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
};

inline constexpr uint8_t kLayerMask = kLayerNumeric | kLayerNumericNested;

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
  ChecksumMismatch,  // the rebuilt bytes do not match the stored CRC-32
  BadMode,        // mode byte is outside the defined set, or unsupported here
  BadVarint,      // varint is truncated or overflows 64 bits
  Truncated,      // stream ended before a declared field/block completed
  BadBlockSize,   // zero, or above kMaxBlockSize / kMaxStreamSize
  SizeMismatch,   // declared sizes disagree with the bytes actually produced
  TrailingData,   // bytes left over after the last block
  Corrupt,        // an entropy backend rejected its own payload
  Io,             // read or write failure on the CLI side
};

constexpr const char* err_name(Err e) {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::BadMagic: return "bad-magic";
    case Err::BadVersion: return "bad-version";
    case Err::BadFlags: return "bad-flags";
    case Err::ChecksumMismatch: return "checksum-mismatch";
    case Err::BadMode: return "bad-mode";
    case Err::BadVarint: return "bad-varint";
    case Err::Truncated: return "truncated";
    case Err::BadBlockSize: return "bad-block-size";
    case Err::SizeMismatch: return "size-mismatch";
    case Err::TrailingData: return "trailing-data";
    case Err::Corrupt: return "corrupt";
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
    case Err::ChecksumMismatch: return "checksum mismatch";
    case Err::BadMode: return "invalid or unsupported block mode";
    case Err::BadVarint: return "malformed varint";
    case Err::Truncated: return "stream truncated";
    case Err::BadBlockSize: return "invalid block size";
    case Err::SizeMismatch: return "declared size does not match payload";
    case Err::TrailingData: return "trailing data after final block";
    case Err::Corrupt: return "compressed payload is corrupt";
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

  // Fixed-width little-endian, for fields whose size must not vary.
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
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
