// dedup.cpp — coarse deduplication: literals and back-references.
//
// Stream layout:
//   varint original_size
//   repeated:
//     0x00 varint len  <len literal bytes>
//     0x01 varint dist varint len        (copy from `dist` bytes back)
//
// Commands are interleaved with the literal bytes rather than kept in a separate
// stream, so the downstream compressor sees them together.
#include "dedup.hpp"

#include <cstring>

#include "chimera/coarse_index.hpp"

namespace chce {
namespace {

// A copy must be long. Short repeats (repeated XML tags, code idioms) are what
// BWT and PPMd exploit best, and replacing them with commands destroys exactly
// the structure those backends rely on: at a 64-byte minimum this stage made xml
// 20.8% WORSE. Only substantial duplicates — whole files inside an archive — are
// worth lifting out.
constexpr uint32_t kMinMatch = 4096;
constexpr uint32_t kMaxMatch = 1u << 20;

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(std::span<const uint8_t> s, size_t& pos, uint64_t& out) {
  out = 0;
  uint32_t shift = 0;
  while (pos < s.size()) {
    const uint8_t b = s[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

void put_literal(std::vector<uint8_t>& out, std::span<const uint8_t> in,
                 size_t from, size_t len) {
  out.push_back(0x00);
  put_varint(out, len);
  out.insert(out.end(), in.begin() + from, in.begin() + from + len);
}

}  // namespace

bool dedup_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  packed.clear();
  if (in.size() < kMinMatch * 2) return false;

  CoarseIndex::Options opt;
  opt.block_len = kMinMatch;
  opt.max_len = kMaxMatch;
  CoarseIndex idx(in, opt);

  put_varint(packed, in.size());

  size_t lit_start = 0;
  uint32_t pos = 0;
  size_t copies = 0;
  size_t copied_bytes = 0;

  while (pos + kMinMatch <= in.size()) {
    idx.insert(pos);
    Match m{};
    if (idx.query(pos, &m) && m.len >= kMinMatch && m.dist >= 1 && m.dist <= pos) {
      if (pos > lit_start) put_literal(packed, in, lit_start, pos - lit_start);
      packed.push_back(0x01);
      put_varint(packed, m.dist);
      put_varint(packed, m.len);

      // Index the positions inside the copy too, so later duplicates can
      // reference this one rather than the original.
      for (uint32_t i = 1; i < m.len && pos + i + kMinMatch <= in.size(); ++i) {
        idx.insert(pos + i);
      }
      pos += m.len;
      lit_start = pos;
      ++copies;
      copied_bytes += m.len;
    } else {
      ++pos;
    }
  }
  if (lit_start < in.size()) {
    put_literal(packed, in, lit_start, in.size() - lit_start);
  }

  // Only engage when the removal is large. A small reduction is a strong signal
  // that the duplicates were short and structural, and the command stream will
  // then compress worse than the original.
  if (copies == 0 || copied_bytes < in.size() / 4) {
    packed.clear();
    return false;
  }
  return true;
}

bool dedup_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t total = 0;
  if (!get_varint(packed, pos, total)) return false;
  if (total > (uint64_t{1} << 40)) return false;

  out.clear();
  out.reserve(static_cast<size_t>(total));

  while (pos < packed.size() && out.size() < total) {
    const uint8_t tag = packed[pos++];
    if (tag == 0x00) {
      uint64_t len = 0;
      if (!get_varint(packed, pos, len)) return false;
      if (pos + len > packed.size()) return false;
      if (out.size() + len > total) return false;
      out.insert(out.end(), packed.begin() + pos, packed.begin() + pos + len);
      pos += static_cast<size_t>(len);
    } else if (tag == 0x01) {
      uint64_t dist = 0, len = 0;
      if (!get_varint(packed, pos, dist)) return false;
      if (!get_varint(packed, pos, len)) return false;
      if (dist == 0 || dist > out.size()) return false;
      if (out.size() + len > total) return false;
      // Byte by byte: the copy may overlap its own output.
      const size_t from = out.size() - static_cast<size_t>(dist);
      for (uint64_t i = 0; i < len; ++i) out.push_back(out[from + i]);
    } else {
      return false;
    }
  }
  return out.size() == total;
}

}  // namespace chce
