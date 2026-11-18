// lz4_wrap.cpp — see the header for why this stores the plaintext and re-encodes.
#include "lz4_wrap.hpp"

#include <cstring>

#include <lz4frame.h>

namespace chce {
namespace {

// Frame magic, little-endian 0x184D2204.
constexpr uint8_t kMagic[4] = {0x04, 0x22, 0x4D, 0x18};
constexpr uint8_t kPayloadMagic[4] = {'L', 'Z', '4', 'S'};

// The grid the search walks. Block size, acceleration/level and block linkage are the three
// settings that change the emitted bytes; content size is the fourth, because a producer that
// knows the length writes it into the frame header and one that streams does not.
constexpr int kBlockSizes[4] = {LZ4F_max64KB, LZ4F_max256KB, LZ4F_max1MB, LZ4F_max4MB};
// The header pins everything but the level, so the level list can be wide.
constexpr int kLevels[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr int kLinked[2] = {1, 0};
constexpr int kContentSize[2] = {1, 0};

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(const std::vector<uint8_t>& v, size_t& pos, uint64_t& out) {
  out = 0;
  int shift = 0;
  while (pos < v.size() && shift <= 63) {
    const uint8_t b = v[pos++];
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

bool decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  LZ4F_dctx* d = nullptr;
  if (LZ4F_isError(LZ4F_createDecompressionContext(&d, LZ4F_VERSION))) return false;
  out.clear();
  size_t src = 0;
  std::vector<uint8_t> chunk(1u << 20);
  bool ok = true;
  while (src < in.size()) {
    size_t dst_size = chunk.size();
    size_t src_size = in.size() - src;
    const size_t r = LZ4F_decompress(d, chunk.data(), &dst_size, in.data() + src, &src_size, nullptr);
    if (LZ4F_isError(r)) { ok = false; break; }
    out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<long>(dst_size));
    src += src_size;
    if (r == 0) break;  // frame complete
    if (src_size == 0 && dst_size == 0) { ok = false; break; }  // no progress
  }
  LZ4F_freeDecompressionContext(d);
  return ok && !out.empty();
}

bool encode(const std::vector<uint8_t>& raw, int block_size, int level, int linked,
            int content_size, std::vector<uint8_t>& out) {
  LZ4F_preferences_t prefs;
  std::memset(&prefs, 0, sizeof(prefs));
  prefs.frameInfo.blockSizeID = static_cast<LZ4F_blockSizeID_t>(block_size);
  prefs.frameInfo.blockMode =
      linked ? LZ4F_blockLinked : LZ4F_blockIndependent;
  prefs.frameInfo.contentSize = content_size ? raw.size() : 0;
  prefs.compressionLevel = level;
  out.resize(LZ4F_compressFrameBound(raw.size(), &prefs));
  const size_t n = LZ4F_compressFrame(out.data(), out.size(), raw.data(), raw.size(), &prefs);
  if (LZ4F_isError(n)) return false;
  out.resize(n);
  return true;
}

}  // namespace

bool lz4_available() { return true; }

bool lz4_likely(const std::vector<uint8_t>& in) {
  // Anywhere, like every other nested transform: a container payload puts its own header first.
  for (size_t i = 0; i + 4 <= in.size(); ++i) {
    if (in[i] == kMagic[0] && std::memcmp(in.data() + i, kMagic, 4) == 0) return true;
  }
  return false;
}

bool lz4_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  if (!lz4_likely(in)) return false;

  std::vector<uint8_t> raw;
  if (!decode(in, raw)) return false;

  // The frame descriptor already states three of the four settings — block size, block mode
  // and whether the content size is present — so only the compression level has to be
  // searched. Reading them is not an optimisation detail: it is the difference between 32
  // re-encodes and 10 on a file that will decline anyway.
  int bs_hint = -1, linked_hint = -1, cs_hint = -1;
  if (in.size() >= 6) {
    const uint8_t flg = in[4], bd = in[5];
    if (((flg >> 6) & 3) == 1) {  // version 01
      linked_hint = ((flg >> 5) & 1) ? 0 : 1;  // bit set means block-independent
      cs_hint = ((flg >> 3) & 1) ? 1 : 0;
      const int bd_id = (bd >> 4) & 7;
      if (bd_id >= 4 && bd_id <= 7) bs_hint = bd_id;
    }
  }

  int hit_bs = -1, hit_lvl = -1, hit_linked = -1, hit_cs = -1;
  std::vector<uint8_t> again;
  const std::vector<int> levels(kLevels, kLevels + sizeof(kLevels) / sizeof(kLevels[0]));
  for (int bs : (bs_hint >= 0 ? std::vector<int>{bs_hint} : std::vector<int>(kBlockSizes, kBlockSizes + 4))) {
    for (int lvl : levels) {
      for (int linked : (linked_hint >= 0 ? std::vector<int>{linked_hint} : std::vector<int>(kLinked, kLinked + 2))) {
        for (int cs : (cs_hint >= 0 ? std::vector<int>{cs_hint} : std::vector<int>(kContentSize, kContentSize + 2))) {
          if (hit_bs >= 0) break;
          if (!encode(raw, bs, lvl, linked, cs, again)) continue;
          if (again == in) { hit_bs = bs; hit_lvl = lvl; hit_linked = linked; hit_cs = cs; }
        }
      }
    }
  }
  if (hit_bs < 0) return false;  // a producer this build cannot reproduce — decline

  payload.clear();
  payload.reserve(raw.size() + 24);
  payload.insert(payload.end(), kPayloadMagic, kPayloadMagic + 4);
  payload.push_back(static_cast<uint8_t>(hit_bs));
  payload.push_back(static_cast<uint8_t>(hit_lvl));
  payload.push_back(static_cast<uint8_t>(hit_linked));
  payload.push_back(static_cast<uint8_t>(hit_cs));
  put_varint(payload, raw.size());
  payload.insert(payload.end(), raw.begin(), raw.end());

  std::vector<uint8_t> check;
  if (!lz4_repack(payload, check) || check != in) {
    payload.clear();
    return false;
  }
  return true;
}

bool lz4_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  if (payload.size() < 9 || std::memcmp(payload.data(), kPayloadMagic, 4) != 0) return false;
  size_t pos = 4;
  const int block_size = payload[pos++];
  const int level = payload[pos++];
  const int linked = payload[pos++];
  const int content_size = payload[pos++];
  if (linked > 1 || content_size > 1 || level < 0 || level > 12) return false;
  uint64_t inflated = 0;
  if (!get_varint(payload, pos, inflated)) return false;
  if (inflated != payload.size() - pos) return false;

  const std::vector<uint8_t> raw(payload.begin() + static_cast<long>(pos), payload.end());
  return encode(raw, block_size, level, linked, content_size, out);
}

}  // namespace chce
