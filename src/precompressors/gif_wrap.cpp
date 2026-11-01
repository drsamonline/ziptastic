// gif_wrap.cpp — GIF LZW <-> palette indices.
//
// Payload layout:
//   varint image_count
//   per image: varint offset, varint lzw_len, varint min_code, varint idx_len
//   per image: index bytes
//   skeleton: the original file with each LZW stream cut out
//
// The skeleton keeps the headers, colour tables, extension blocks and the
// sub-block framing, so only the compressed image data is replaced.
#include "gif_wrap.hpp"

#include <cstring>

namespace chce {
namespace {

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

struct BitReader {
  const uint8_t* p;
  size_t n;
  size_t bit = 0;
  bool ok = true;
  uint32_t get(uint32_t w) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < w; ++i) {
      const size_t b = bit + i;
      if (b / 8 >= n) {
        ok = false;
        return 0;
      }
      v |= (static_cast<uint32_t>((p[b / 8] >> (b % 8)) & 1u)) << i;
    }
    bit += w;
    return v;
  }
};

struct BitWriter {
  std::vector<uint8_t> out;
  uint32_t acc = 0, nb = 0;
  void put(uint32_t v, uint32_t w) {
    for (uint32_t i = 0; i < w; ++i) {
      acc |= ((v >> i) & 1u) << nb;
      if (++nb == 8) {
        out.push_back(static_cast<uint8_t>(acc));
        acc = 0;
        nb = 0;
      }
    }
  }
  void flush() {
    if (nb) {
      out.push_back(static_cast<uint8_t>(acc));
      acc = 0;
      nb = 0;
    }
  }
};

bool lzw_decode(const uint8_t* data, size_t n, uint32_t min_code,
                std::vector<uint8_t>& idx) {
  BitReader br{data, n};
  const uint32_t clear = 1u << min_code, end = clear + 1;
  uint32_t code_size = min_code + 1, next = clear + 2;
  std::vector<std::vector<uint8_t>> table(4096);
  auto reset = [&] {
    for (uint32_t i = 0; i < clear; ++i) {
      table[i].assign(1, static_cast<uint8_t>(i));
    }
    code_size = min_code + 1;
    next = clear + 2;
  };
  reset();
  std::vector<uint8_t> prev;
  while (true) {
    const uint32_t code = br.get(code_size);
    if (!br.ok) return false;
    if (code == clear) {
      reset();
      prev.clear();
      continue;
    }
    if (code == end) break;
    std::vector<uint8_t> entry;
    if (code < next && !table[code].empty()) {
      entry = table[code];
    } else if (!prev.empty()) {
      entry = prev;
      entry.push_back(prev[0]);
    } else {
      return false;
    }
    idx.insert(idx.end(), entry.begin(), entry.end());
    if (!prev.empty() && next < 4096) {
      std::vector<uint8_t> e = prev;
      e.push_back(entry[0]);
      table[next++] = std::move(e);
    }
    prev = std::move(entry);
    if (next == (1u << code_size) && code_size < 12) ++code_size;
  }
  return true;
}

// A standard GIF LZW encoder. Whether it reproduces a given file depends on that
// file's encoder, which is why every pack is verified before use.
std::vector<uint8_t> lzw_encode(const std::vector<uint8_t>& idx, uint32_t min_code) {
  BitWriter bw;
  const uint32_t clear = 1u << min_code, end = clear + 1;
  uint32_t code_size = min_code + 1, next = clear + 2;
  std::vector<int32_t> dict(4096 * 256, -1);  // (prefix, byte) -> code
  bw.put(clear, code_size);
  if (idx.empty()) {
    bw.put(end, code_size);
    bw.flush();
    return bw.out;
  }
  int32_t cur = idx[0];
  for (size_t i = 1; i < idx.size(); ++i) {
    const uint8_t c = idx[i];
    int32_t& slot = dict[static_cast<size_t>(cur) * 256 + c];
    if (slot >= 0) {
      cur = slot;
      continue;
    }
    bw.put(static_cast<uint32_t>(cur), code_size);
    if (next < 4096) {
      slot = static_cast<int32_t>(next++);
      if (next > (1u << code_size) && code_size < 12) ++code_size;
    } else {
      bw.put(clear, code_size);
      std::fill(dict.begin(), dict.end(), -1);
      code_size = min_code + 1;
      next = clear + 2;
    }
    cur = c;
  }
  bw.put(static_cast<uint32_t>(cur), code_size);
  bw.put(end, code_size);
  bw.flush();
  return bw.out;
}

struct Image {
  uint32_t offset = 0;   // where the LZW sub-block chain starts
  uint32_t lzw_len = 0;  // total bytes of LZW payload across sub-blocks
  uint32_t min_code = 0;
  std::vector<uint8_t> idx;
};

}  // namespace

bool gif_available() { return true; }

bool gif_likely(std::span<const uint8_t> in) {
  return in.size() > 13 && std::memcmp(in.data(), "GIF8", 4) == 0;
}

bool gif_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  if (!gif_likely(in)) return false;

  std::vector<Image> images;
  size_t pos = 13;
  if (in[10] & 0x80) pos += 3u * (1u << ((in[10] & 7) + 1));
  if (pos >= in.size()) return false;

  while (pos < in.size()) {
    const uint8_t b = in[pos];
    if (b == 0x21) {  // extension block
      pos += 2;
      while (pos < in.size() && in[pos]) pos += in[pos] + 1;
      ++pos;
    } else if (b == 0x2C) {  // image descriptor
      if (pos + 10 >= in.size()) return false;
      const uint8_t flags = in[pos + 9];
      pos += 10;
      if (flags & 0x80) pos += 3u * (1u << ((flags & 7) + 1));
      if (pos >= in.size()) return false;
      Image img;
      img.min_code = in[pos++];
      img.offset = static_cast<uint32_t>(pos);

      // Concatenate the sub-block chain to decode, but remember its extent.
      std::vector<uint8_t> lzw;
      const size_t chain_start = pos;
      while (pos < in.size() && in[pos]) {
        const uint8_t n = in[pos++];
        if (pos + n > in.size()) return false;
        lzw.insert(lzw.end(), in.begin() + pos, in.begin() + pos + n);
        pos += n;
      }
      if (pos >= in.size()) return false;
      ++pos;  // terminator
      img.lzw_len = static_cast<uint32_t>(pos - chain_start);
      if (img.min_code < 2 || img.min_code > 8) return false;
      if (!lzw_decode(lzw.data(), lzw.size(), img.min_code, img.idx)) return false;
      if (lzw_encode(img.idx, img.min_code) != lzw) return false;  // not reproducible
      images.push_back(std::move(img));
    } else {
      break;  // trailer or unknown
    }
  }
  if (images.empty()) return false;

  packed.clear();
  put_varint(packed, images.size());
  for (const Image& im : images) {
    put_varint(packed, im.offset);
    put_varint(packed, im.lzw_len);
    put_varint(packed, im.min_code);
    put_varint(packed, im.idx.size());
  }
  for (const Image& im : images) packed.insert(packed.end(), im.idx.begin(), im.idx.end());

  // Skeleton: the file with each LZW sub-block chain cut out.
  size_t cur = 0;
  for (const Image& im : images) {
    packed.insert(packed.end(), in.begin() + cur, in.begin() + im.offset);
    cur = im.offset + im.lzw_len;
  }
  packed.insert(packed.end(), in.begin() + cur, in.end());
  return true;
}

bool gif_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  size_t pos = 0;
  uint64_t count = 0;
  if (!get_varint(packed, pos, count) || count == 0 || count > 100000) return false;

  struct Entry {
    uint32_t offset, lzw_len, min_code, idx_len;
  };
  std::vector<Entry> es(static_cast<size_t>(count));
  for (auto& e : es) {
    uint64_t a = 0, b = 0, c = 0, d = 0;
    if (!get_varint(packed, pos, a) || !get_varint(packed, pos, b) ||
        !get_varint(packed, pos, c) || !get_varint(packed, pos, d)) {
      return false;
    }
    e.offset = static_cast<uint32_t>(a);
    e.lzw_len = static_cast<uint32_t>(b);
    e.min_code = static_cast<uint32_t>(c);
    e.idx_len = static_cast<uint32_t>(d);
  }
  std::vector<std::vector<uint8_t>> idx(es.size());
  for (size_t i = 0; i < es.size(); ++i) {
    if (pos + es[i].idx_len > packed.size()) return false;
    idx[i].assign(packed.begin() + pos, packed.begin() + pos + es[i].idx_len);
    pos += es[i].idx_len;
  }

  out.clear();
  size_t skel = pos, cur = 0;
  for (size_t i = 0; i < es.size(); ++i) {
    const Entry& e = es[i];
    if (e.offset < cur) return false;
    const size_t copy = e.offset - cur;
    if (skel + copy > packed.size()) return false;
    out.insert(out.end(), packed.begin() + skel, packed.begin() + skel + copy);
    skel += copy;

    const std::vector<uint8_t> lzw = lzw_encode(idx[i], e.min_code);
    // Re-emit the sub-block chain and check it occupies exactly the space the
    // original did. Comparing `lzw.size() + 1` was wrong: the chain also carries
    // a length byte per 255-byte chunk, so the framing never matched and every
    // unpack failed.
    std::vector<uint8_t> chain;
    size_t p = 0;
    while (p < lzw.size()) {
      const size_t n = std::min<size_t>(255, lzw.size() - p);
      chain.push_back(static_cast<uint8_t>(n));
      chain.insert(chain.end(), lzw.begin() + p, lzw.begin() + p + n);
      p += n;
    }
    chain.push_back(0);
    if (chain.size() != e.lzw_len) return false;
    out.insert(out.end(), chain.begin(), chain.end());
    cur = e.offset + e.lzw_len;
  }
  out.insert(out.end(), packed.begin() + skel, packed.end());
  return true;
}

}  // namespace chce
