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
inline constexpr uint8_t kFormatVersion = 1;

// Default block size. The prompt starts at 256 KB; Phase 12 tunes it.
inline constexpr size_t kDefaultBlockSize = 256u * 1024u;

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
};
inline constexpr uint8_t kFlagMask = kFlagCoarse | kFlagMaxRatio | kFlagPrecomp;

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
};

constexpr uint8_t mode_byte(Mode m) { return static_cast<uint8_t>(m); }

constexpr bool mode_is_known(uint8_t b) { return b <= mode_byte(Mode::Ppmd); }

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
  BadMode,        // mode byte is outside the defined set, or unsupported here
  BadVarint,      // varint is truncated or overflows 64 bits
  Truncated,      // stream ended before a declared field/block completed
  BadBlockSize,   // zero, or above kMaxBlockSize / kMaxStreamSize
  SizeMismatch,   // declared sizes disagree with the bytes actually produced
  TrailingData,   // bytes left over after the last block
  Io,             // read or write failure on the CLI side
};

constexpr const char* err_name(Err e) {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::BadMagic: return "bad-magic";
    case Err::BadVersion: return "bad-version";
    case Err::BadFlags: return "bad-flags";
    case Err::BadMode: return "bad-mode";
    case Err::BadVarint: return "bad-varint";
    case Err::Truncated: return "truncated";
    case Err::BadBlockSize: return "bad-block-size";
    case Err::SizeMismatch: return "size-mismatch";
    case Err::TrailingData: return "trailing-data";
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
    case Err::BadMode: return "invalid or unsupported block mode";
    case Err::BadVarint: return "malformed varint";
    case Err::Truncated: return "stream truncated";
    case Err::BadBlockSize: return "invalid block size";
    case Err::SizeMismatch: return "declared size does not match payload";
    case Err::TrailingData: return "trailing data after final block";
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
