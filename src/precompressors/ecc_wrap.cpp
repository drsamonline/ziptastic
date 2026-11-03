// ecc_wrap.cpp — Reed-Solomon product code for CD sectors, regenerated on decode.
//
// The algorithm is the Yellow Book's RS(26,24) product code over GF(2^8) with primitive
// polynomial 0x11D, written from the standard rather than copied: ziptastic is MIT and
// the reference implementations are GPL. It is checked against a real image produced by
// mkpsxiso — see docs/ECM_TEST_MATERIAL.md — so "written from the standard" is not taken
// on trust.
#include "ecc_wrap.hpp"

#include <cstring>

namespace chce {
namespace {

constexpr size_t kSector = 2352;
constexpr size_t kEccAt = 2076;  // 12 sync + 4 header + 8 subheader + 2048 data + 4 EDC
constexpr size_t kEccLen = 276;  // 172 P + 104 Q

struct Tables {
  uint8_t f[256];
  uint8_t b[256];
  uint32_t edc[256];
  Tables() {
    for (unsigned i = 0; i < 256; ++i) {
      const unsigned j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
      f[i] = static_cast<uint8_t>(j);
      b[i ^ j] = static_cast<uint8_t>(i);
      uint32_t e = i;
      for (int k = 0; k < 8; ++k) e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
      edc[i] = e;
    }
  }
};

const Tables& tables() {
  static const Tables t;
  return t;
}

// The generic product-code pass. `get(index)` returns the codeword byte at a logical
// position: the first four are the address field, the rest come from the sector.
//
// It takes a getter rather than a flat buffer because of the Q pass. Q is computed over
// 52 x 43 = 2,236 bytes, which is LARGER than the 2,064-byte data region, because it
// also covers the 172 P parity bytes written moments earlier. Reading from the sector
// itself is what makes that work; a private 2,064-byte copy computes Q over the wrong
// bytes and produces 104 parity bytes that still look plausible — which is exactly what
// the first version did, and the per-sector check caught it.
template <typename Get>
void ecc_pass(Get get, uint32_t major_count, uint32_t minor_count, uint32_t major_mult,
              uint32_t minor_inc, uint8_t* dest) {
  const Tables& t = tables();
  const uint32_t size = major_count * minor_count;
  for (uint32_t major = 0; major < major_count; ++major) {
    uint32_t index = (major >> 1) * major_mult + (major & 1);
    uint8_t a = 0;
    uint8_t bb = 0;
    for (uint32_t minor = 0; minor < minor_count; ++minor) {
      const uint8_t temp = get(index);
      index += minor_inc;
      if (index >= size) index -= size;
      a ^= temp;
      bb ^= temp;
      a = t.f[a];
    }
    a = t.b[t.f[a] ^ bb];
    dest[major] = a;
    dest[major + major_count] = static_cast<uint8_t>(a ^ bb);
  }
}

// Mode 1 and Mode 2 Form 1 carry parity; Mode 2 Form 2 does not (those 276 bytes are
// data), and anything else is not a sector this transform understands.
enum class Kind : uint8_t { Ecc, Whole, Unsupported };

Kind sector_kind(const uint8_t* sec) {
  const uint8_t mode = sec[15];
  if (mode == 1) return Kind::Ecc;
  if (mode == 2) return ((sec[18] & 0x20) != 0) ? Kind::Whole : Kind::Ecc;
  return Kind::Unsupported;
}

// kinds[] holds 0 = parity regenerated with the header address, 1 = sector kept whole,
// 2 = parity regenerated with a zero address.
void compute_ecc(uint8_t* sec, bool zero_address) {
  // The address field is the sector header (Yellow Book) or four zero bytes; both
  // conventions exist in the wild and a given image matches only one.
  uint8_t addr[4];
  if (zero_address) {
    std::memset(addr, 0, 4);
  } else {
    std::memcpy(addr, sec + 12, 4);
  }
  // Logical index 0..3 is the address, everything above is the sector from offset 16.
  // For Q the range reaches past the data into the P parity, so P is written into the
  // sector before Q is computed.
  auto get = [&](uint32_t index) -> uint8_t {
    return index < 4 ? addr[index] : sec[16 + index - 4];
  };
  uint8_t* dest = sec + kEccAt;
  ecc_pass(get, 86, 24, 2, 86, dest);         // P: 172 bytes
  ecc_pass(get, 52, 43, 86, 88, dest + 172);  // Q: 104 bytes, over the data AND P
}

}  // namespace

bool ecc_available() { return true; }

bool ecc_likely(std::span<const uint8_t> in) {
  static const uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  if (in.size() < kSector || in.size() % kSector != 0) return false;
  return std::memcmp(in.data(), kSync, sizeof(kSync)) == 0;
}

bool ecc_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  if (!ecc_likely(in)) return false;
  const size_t sectors = in.size() / kSector;

  std::vector<uint8_t> kinds(sectors, 0);
  size_t ecc_sectors = 0;
  for (size_t i = 0; i < sectors; ++i) {
    uint8_t sec[kSector];
    std::memcpy(sec, in.data() + i * kSector, kSector);
    const Kind k = sector_kind(sec);
    if (k == Kind::Unsupported) return false;
    if (k == Kind::Whole) {
      kinds[i] = 1;
      continue;
    }
    // The conformance check, per sector: recompute and compare. A real image must match;
    // a mismatch means either the image is damaged or this implementation is wrong, and
    // both are reasons to decline rather than guess.
    uint8_t probe[kSector];
    std::memcpy(probe, sec, kSector);
    compute_ecc(probe, /*zero_address=*/false);
    if (std::memcmp(probe + kEccAt, sec + kEccAt, kEccLen) != 0) {
      std::memcpy(probe, sec, kSector);
      compute_ecc(probe, /*zero_address=*/true);
      if (std::memcmp(probe + kEccAt, sec + kEccAt, kEccLen) != 0) return false;
      kinds[i] = 2;
    }
    ++ecc_sectors;
  }
  if (ecc_sectors == 0) return false;  // nothing to regenerate

  packed.clear();
  packed.reserve(in.size());
  uint64_t n = sectors;
  while (n >= 0x80) {
    packed.push_back(static_cast<uint8_t>(n) | 0x80u);
    n >>= 7;
  }
  packed.push_back(static_cast<uint8_t>(n));
  packed.insert(packed.end(), kinds.begin(), kinds.end());
  for (size_t i = 0; i < sectors; ++i) {
    const uint8_t* sec = in.data() + i * kSector;
    const size_t keep = (kinds[i] == 1) ? kSector : kEccAt;
    packed.insert(packed.end(), sec, sec + keep);
  }
  return packed.size() < in.size();
}

bool ecc_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t sectors = 0;
  uint32_t shift = 0;
  while (pos < packed.size()) {
    const uint8_t b = packed[pos++];
    if (shift > 63) return false;
    sectors |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) break;
    shift += 7;
  }
  if (sectors == 0 || sectors > (1u << 26)) return false;
  if (pos + sectors > packed.size()) return false;
  // The output is 2352 bytes per declared sector, so the sector count is a declared
  // number that sizes an allocation — exactly the class of bug fixed twice already
  // (block_count, dedup). The expansion ceiling in decompress() does not help: it is
  // checked after this allocation has happened.
  //
  // The bound is structural rather than a guess: a legitimate payload carries either
  // 2076 or 2352 bytes of sector per entry, plus one kind byte, so it cannot be shorter
  // than `sectors * 2077`. Without this check a 64 MB payload can declare 2^26 sectors
  // and ask for 158 GB.
  if (static_cast<uint64_t>(packed.size()) <
      static_cast<uint64_t>(sectors) * (kEccAt + 1)) {
    return false;
  }
  const uint8_t* kinds = packed.data() + pos;
  pos += static_cast<size_t>(sectors);

  out.assign(static_cast<size_t>(sectors) * kSector, 0);
  for (size_t i = 0; i < sectors; ++i) {
    const uint8_t kind = kinds[i];
    const size_t keep = (kind == 1) ? kSector : kEccAt;
    if (pos + keep > packed.size()) return false;
    uint8_t* sec = out.data() + i * kSector;
    std::memcpy(sec, packed.data() + pos, keep);
    pos += keep;
    if (kind != 1) compute_ecc(sec, /*zero_address=*/kind == 2);
  }
  return pos == packed.size();
}

}  // namespace chce
