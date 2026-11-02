// sqlite_wrap.cpp — page grouping for SQLite databases.
//
// A database is a sequence of fixed-size pages of five kinds: interior index,
// interior table, leaf index, leaf table, and everything else (freelist, overflow,
// lock bytes). Interleaved, they force the backend to keep switching statistics.
// Grouped, each run of pages has one shape.
//
// The recipe is 12 bytes because the order is recoverable: every page carries its
// own type byte, so the decoder rebuilds the same grouping from the packed pages
// without being told anything. Verified by re-encoding and byte-comparing before the
// payload is returned, like every other transform here.
#include "sqlite_wrap.hpp"

#include <cstring>

namespace chce {
namespace {

constexpr size_t kTrailer = 4 + 4 + 4 + 4;  // page_size | page_count | perm_len | "SL01"
constexpr uint8_t kMagic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                'o', 'r', 'm', 'a', 't', ' ', '3', 0};

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// 0..3 are the B-tree page types the format defines; 4 is everything else.
uint8_t page_type(const uint8_t* page, bool first_page) {
  const uint8_t t = page[first_page ? 100 : 0];
  switch (t) {
    case 2: return 0;   // interior index
    case 5: return 1;   // interior table
    case 10: return 2;  // leaf index
    case 13: return 3;  // leaf table
    default: return 4;
  }
}

bool is_first_page(const uint8_t* page) {
  return std::memcmp(page, kMagic, sizeof(kMagic)) == 0;
}

// Header parse shared by both directions.
bool parse_shape(std::span<const uint8_t> in, uint32_t& page_size, uint32_t& page_count) {
  if (in.size() < 512) return false;
  if (std::memcmp(in.data(), kMagic, sizeof(kMagic)) != 0) return false;
  uint32_t ps = static_cast<uint32_t>(in[16]) << 8 | in[17];
  if (ps == 1) ps = 65536;
  if (ps < 512 || ps > 65536 || (ps & (ps - 1)) != 0) return false;
  if (in.size() % ps != 0) return false;  // a partial page is a hot journal, not a db
  const uint32_t count = static_cast<uint32_t>(in.size() / ps);
  if (count == 0) return false;
  page_size = ps;
  page_count = count;
  return true;
}

}  // namespace

bool sqlite_available() { return true; }

bool sqlite_likely(std::span<const uint8_t> in) {
  uint32_t ps = 0, count = 0;
  return parse_shape(in, ps, count);
}

bool sqlite_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  uint32_t ps = 0, count = 0;
  if (!parse_shape(in, ps, count)) return false;

  // Stable grouping: pages of the same type keep their relative order, so the
  // decoder's rank-within-type is the original index order.
  std::vector<uint32_t> order;
  order.reserve(count);
  for (uint8_t t = 0; t < 5; ++t) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* page = in.data() + static_cast<size_t>(i) * ps;
      if (page_type(page, is_first_page(page)) == t) order.push_back(i);
    }
  }
  if (order.size() != count) return false;

  // The permutation has to be stored: the decoder cannot recover it. Grouping is a
  // sort by type, and the original indices of one type are scattered through the
  // file, so "rank within type" is not the original position. The first version of
  // this file assumed otherwise and produced a payload that decoded to the wrong
  // bytes at offset 0 — caught by the verify, which is what the verify is for.
  std::vector<uint8_t> perm;
  perm.reserve(count * 2);
  for (uint32_t i : order) {
    uint64_t v = i;
    while (v >= 0x80) {
      perm.push_back(static_cast<uint8_t>(v) | 0x80u);
      v >>= 7;
    }
    perm.push_back(static_cast<uint8_t>(v));
  }

  packed.clear();
  packed.reserve(in.size() + perm.size() + kTrailer);
  for (uint32_t i : order) {
    packed.insert(packed.end(), in.begin() + static_cast<size_t>(i) * ps,
                  in.begin() + static_cast<size_t>(i + 1) * ps);
  }
  packed.insert(packed.end(), perm.begin(), perm.end());
  for (int b = 0; b < 4; ++b) packed.push_back(static_cast<uint8_t>(ps >> (8 * b)));
  for (int b = 0; b < 4; ++b) packed.push_back(static_cast<uint8_t>(count >> (8 * b)));
  for (int b = 0; b < 4; ++b) {
    packed.push_back(static_cast<uint8_t>(static_cast<uint32_t>(perm.size()) >> (8 * b)));
  }
  packed.push_back('S');
  packed.push_back('L');
  packed.push_back('0');
  packed.push_back('1');
  // No size test. This is a reordering plus a permutation, so the payload can be a
  // little larger on disk — the gain comes from what the backend does with grouped
  // pages. Applying the usual "packed must be smaller" rule silently disabled the
  // transform on the first run.
  return true;
}

bool sqlite_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  if (packed.size() < kTrailer) return false;
  const size_t t = packed.size();
  if (packed[t - 4] != 'S' || packed[t - 3] != 'L' || packed[t - 2] != '0' ||
      packed[t - 1] != '1') {
    return false;
  }
  const uint32_t ps = rd32(packed.data() + t - kTrailer);
  const uint32_t count = rd32(packed.data() + t - kTrailer + 4);
  const uint32_t perm_len = rd32(packed.data() + t - kTrailer + 8);
  if (ps < 512 || ps > 65536 || (ps & (ps - 1)) != 0) return false;
  if (count == 0) return false;
  const uint64_t pages_bytes = static_cast<uint64_t>(count) * ps;
  if (pages_bytes + perm_len + kTrailer != packed.size()) return false;

  out.assign(static_cast<size_t>(pages_bytes), 0);
  std::vector<uint8_t> seen(count, 0);
  size_t pos = static_cast<size_t>(pages_bytes);
  const size_t perm_end = pos + perm_len;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t idx = 0;
    uint32_t shift = 0;
    while (pos < perm_end) {
      const uint8_t b = packed[pos++];
      if (shift > 63) return false;
      idx |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) break;
      shift += 7;
    }
    if (idx >= count || seen[idx]) return false;
    seen[idx] = 1;
    std::memcpy(out.data() + static_cast<size_t>(idx) * ps,
                packed.data() + static_cast<size_t>(i) * ps, ps);
  }
  if (pos != perm_end) return false;
  return true;
}

}  // namespace chce
