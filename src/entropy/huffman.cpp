// huffman.cpp — canonical Huffman, static and adaptive, over block bytes.
#include "chimera/huffman.hpp"

#include <algorithm>
#include <cstring>

namespace chce {
namespace {

// --- bit I/O ---------------------------------------------------------------

class BitWriter {
 public:
  explicit BitWriter(ByteWriter& w) : w_(w) {}

  void put(uint32_t code, uint32_t len) {
    for (uint32_t i = len; i-- > 0;) {
      acc_ = (acc_ << 1) | ((code >> i) & 1u);
      if (++bits_ == 8) {
        w_.u8(static_cast<uint8_t>(acc_));
        acc_ = 0;
        bits_ = 0;
      }
    }
  }

  void flush() {
    if (bits_ > 0) {
      w_.u8(static_cast<uint8_t>(acc_ << (8 - bits_)));
      acc_ = 0;
      bits_ = 0;
    }
  }

 private:
  ByteWriter& w_;
  uint32_t acc_ = 0;
  uint32_t bits_ = 0;
};

class BitReader {
 public:
  explicit BitReader(ByteReader& r) : r_(r) {}

  // Returns the next bit, or sets ok_ false when the stream is exhausted.
  uint32_t bit() {
    if (bits_ == 0) {
      uint8_t b = 0;
      if (!r_.u8(b)) {
        ok_ = false;
        return 0;
      }
      acc_ = b;
      bits_ = 8;
    }
    const uint32_t v = (acc_ >> (bits_ - 1)) & 1u;
    --bits_;
    return v;
  }

  bool ok() const { return ok_; }

 private:
  ByteReader& r_;
  uint32_t acc_ = 0;
  uint32_t bits_ = 0;
  bool ok_ = true;
};

// --- tree helpers ----------------------------------------------------------

// A binary decode tree, rebuilt whenever the codes change.
struct DecodeTree {
  // children[2*i] / children[2*i+1]; negative child means "symbol = -child-1".
  std::vector<int32_t> child;
  bool valid = false;

  void build(const HuffCodes& c) {
    child.assign(2, 0);  // node 0 = root, both slots empty
    int32_t nodes = 1;
    valid = true;
    for (uint32_t s = 0; s < 256; ++s) {
      if (c.len[s] == 0) continue;
      int32_t node = 0;
      for (uint32_t i = c.len[s]; i-- > 0;) {
        const uint32_t b = (c.code[s] >> i) & 1u;
        // Index arithmetic, not a reference: resize() below would invalidate a
        // reference into `child`, and writing through it corrupts freed memory.
        // (That bug let 1- and 2-symbol blocks pass while anything needing an
        // interior node failed.)
        const size_t slot = static_cast<size_t>(node) * 2 + b;
        if (i == 0) {
          child[slot] = -static_cast<int32_t>(s) - 1;  // leaf
        } else {
          if (child[slot] <= 0) {
            child.resize(child.size() + 2, 0);
            child[slot] = nodes;
            ++nodes;
          }
          node = child[slot];
        }
      }
    }
  }

  int32_t walk(BitReader& br) const {
    if (!valid) return -1;
    int32_t node = 0;
    for (uint32_t depth = 0; depth < kMaxCodeLen + 1; ++depth) {
      const uint32_t b = br.bit();
      if (!br.ok()) return -1;
      const int32_t next = child[static_cast<size_t>(node) * 2 + b];
      if (next < 0) return -next - 1;
      if (next == 0) return -1;  // dead end: corrupt stream
      node = next;
    }
    return -1;
  }
};

void flat_codes(HuffCodes& c) {
  for (uint32_t s = 0; s < 256; ++s) {
    c.len[s] = 8;
    c.code[s] = s;
  }
}

}  // namespace

void huff_build(const uint32_t freq[256], HuffCodes& out) {
  std::memset(&out, 0, sizeof(out));

  uint32_t used = 0;
  uint64_t total = 0;
  for (uint32_t s = 0; s < 256; ++s) {
    if (freq[s]) ++used;
    total += freq[s];
  }
  if (total == 0) {
    flat_codes(out);
    return;
  }
  if (used <= 1) {
    // One symbol: give it a 1-bit code so the decoder always consumes a bit and
    // the stream cannot be ambiguous.
    for (uint32_t s = 0; s < 256; ++s) {
      if (freq[s]) {
        out.len[s] = 1;
        out.code[s] = 0;
      }
    }
    return;
  }

  // Huffman by repeated merging, with deterministic tie-breaking: (frequency,
  // order of creation). Using symbol order as the secondary key makes the tree
  // identical on both sides for identical counts.
  struct Node {
    uint64_t freq;
    int32_t parent;
    uint32_t order;
  };
  std::vector<Node> nodes;
  nodes.reserve(512);
  for (uint32_t s = 0; s < 256; ++s) {
    if (freq[s]) nodes.push_back(Node{freq[s], -1, s});
  }

  // Simple O(n^2) merge: 256 symbols at most, so this is trivial and obviously
  // correct.
  while (true) {
    int32_t a = -1, b = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].parent != -1) continue;
      if (a < 0 || nodes[i].freq < nodes[a].freq ||
          (nodes[i].freq == nodes[a].freq && nodes[i].order < nodes[a].order)) {
        b = a;
        a = static_cast<int32_t>(i);
      } else if (b < 0 || nodes[i].freq < nodes[b].freq ||
                 (nodes[i].freq == nodes[b].freq && nodes[i].order < nodes[b].order)) {
        b = static_cast<int32_t>(i);
      }
    }
    if (b < 0) break;
    const uint32_t order = 256 + static_cast<uint32_t>(nodes.size());
    nodes.push_back(Node{nodes[a].freq + nodes[b].freq, -1, order});
    const int32_t parent = static_cast<int32_t>(nodes.size()) - 1;
    nodes[a].parent = parent;
    nodes[b].parent = parent;
    if (nodes.size() > 512) break;  // cannot happen with 256 leaves
  }

  // Depth of each leaf, capped. Leaves are the first `used` nodes.
  uint8_t len[256] = {};
  bool too_deep = false;
  for (uint32_t s = 0; s < 256; ++s) {
    if (!freq[s]) continue;
    uint32_t depth = 0;
    int32_t n = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i].order == s) {
        n = static_cast<int32_t>(i);
        break;
      }
    }
    while (n >= 0 && nodes[static_cast<size_t>(n)].parent >= 0) {
      n = nodes[static_cast<size_t>(n)].parent;
      ++depth;
    }
    if (depth == 0) depth = 1;
    if (depth > kMaxCodeLen) too_deep = true;
    len[s] = static_cast<uint8_t>(depth);
  }
  if (too_deep) {
    flat_codes(out);
    return;
  }

  // Canonical assignment: sort by (length, symbol), then hand out codes in
  // increasing order. Both sides run the same code, so the assignment is fixed.
  for (uint32_t s = 0; s < 256; ++s) out.len[s] = len[s];
  uint32_t code = 0;
  for (uint32_t bits = 1; bits <= kMaxCodeLen; ++bits) {
    for (uint32_t s = 0; s < 256; ++s) {
      if (out.len[s] != bits) continue;
      out.code[s] = code;
      ++code;
    }
    code <<= 1;
  }
}

void huff_static_encode(std::span<const uint8_t> block, ByteWriter& w) {
  uint32_t freq[256] = {};
  for (uint8_t b : block) ++freq[b];

  HuffCodes c;
  huff_build(freq, c);
  for (uint32_t s = 0; s < 256; ++s) w.u8(c.len[s]);

  BitWriter bw(w);
  for (uint8_t b : block) bw.put(c.code[b], c.len[b]);
  bw.flush();
}

Err huff_static_decode(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out) {
  HuffCodes c;
  for (uint32_t s = 0; s < 256; ++s) {
    uint8_t l = 0;
    if (!r.u8(l)) return Err::Truncated;
    if (l > kMaxCodeLen) return Err::BadMode;
    c.len[s] = l;
  }
  // Rebuild the canonical codes from the transmitted lengths.
  uint32_t code = 0;
  for (uint32_t bits = 1; bits <= kMaxCodeLen; ++bits) {
    for (uint32_t s = 0; s < 256; ++s) {
      if (c.len[s] != bits) continue;
      c.code[s] = code;
      ++code;
    }
    code <<= 1;
  }

  DecodeTree tree;
  tree.build(c);

  BitReader br(r);
  const size_t start = out.size();
  while (out.size() - start < out_size) {
    const int32_t sym = tree.walk(br);
    if (sym < 0) return Err::Corrupt;
    out.push_back(static_cast<uint8_t>(sym));
  }
  if (!br.ok() && out.size() - start != out_size) return Err::Truncated;
  return Err::Ok;
}

namespace {

// The adaptive model: counts, halved every rebuild so the tree follows the data
// rather than averaging over the whole block.
struct AdaptiveModel {
  uint32_t count[256] = {};
  HuffCodes codes;
  DecodeTree tree;

  void init() {
    for (uint32_t s = 0; s < 256; ++s) count[s] = 1;  // flat start
    huff_build(count, codes);
    tree.build(codes);
  }

  void rebuild() {
    for (uint32_t s = 0; s < 256; ++s) count[s] = (count[s] + 1) >> 1;
    huff_build(count, codes);
    tree.build(codes);
  }
};

}  // namespace

void huff_adaptive_encode(std::span<const uint8_t> block, ByteWriter& w) {
  AdaptiveModel m;
  m.init();
  BitWriter bw(w);

  uint32_t since = 0;
  for (uint8_t b : block) {
    bw.put(m.codes.code[b], m.codes.len[b]);
    ++m.count[b];
    if (++since == kAdaptiveRebuild) {
      m.rebuild();
      since = 0;
    }
  }
  bw.flush();
}

Err huff_adaptive_decode(ByteReader& r, uint32_t out_size, std::vector<uint8_t>& out) {
  AdaptiveModel m;
  m.init();
  BitReader br(r);

  const size_t start = out.size();
  uint32_t since = 0;
  while (out.size() - start < out_size) {
    const int32_t sym = m.tree.walk(br);
    if (sym < 0) return Err::Corrupt;
    out.push_back(static_cast<uint8_t>(sym));
    ++m.count[sym];
    if (++since == kAdaptiveRebuild) {
      m.rebuild();
      since = 0;
    }
  }
  return Err::Ok;
}

}  // namespace chce
