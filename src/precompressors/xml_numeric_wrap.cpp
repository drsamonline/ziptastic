// xml_numeric_wrap.cpp — spreadsheet XML cell values as delta + zigzag + varint.
//
// See the header for the measurements and the two design decisions they settled.
// The one thing to hold on to while reading this: encoder and decoder must derive
// the same column id for every cell, and they do it with the same scanner over
// two buffers that differ only inside <v> tags — the digits on the encode side,
// empty on the decode side. Anything the scanner cannot reproduce exactly is
// declined at pack time, and the caller byte-compares the round trip on top of
// that, so a bug here costs ratio, never data.
#include "xml_numeric_wrap.hpp"

#include <cstring>
#include <string>

#include "numeric_common.hpp"

namespace chce {
namespace {

using namespace numdetail;

// Column id is derived from the cell's `r` attribute when it has one (A1 -> 0,
// B1 -> 1, AA1 -> 26) and from the cell's ordinal position in its row when it
// does not. Ordinal is the honest fallback: a reader that ignores `r` sees cells
// in exactly that order.
constexpr uint32_t kMaxCols = 16384;

// A cell whose value sits between <v> and </v>, with the column it belongs to.
struct CellValue {
  size_t val_begin = 0;  // first byte of the digits
  size_t val_end = 0;    // one past the last byte of the digits
  uint32_t col = 0;
};

bool starts_with(const std::span<const uint8_t> b, size_t at, const char* lit) {
  const size_t n = std::strlen(lit);
  if (at + n > b.size()) return false;
  return std::memcmp(b.data() + at, lit, n) == 0;
}

// Finds every <c ...><v>digits</v> in document order. Works identically on the
// original (digits present) and on the skeleton (digits removed), which is what
// keeps encoder and decoder in agreement.
bool scan_cells(const std::span<const uint8_t> b, std::vector<CellValue>& out) {
  out.clear();
  size_t i = 0;
  uint32_t ordinal = 0;
  while (i + 2 < b.size()) {
    if (b[i] != '<') {
      ++i;
      continue;
    }
    if (starts_with(b, i, "<row")) {
      ordinal = 0;
      i += 4;
      continue;
    }
    // A cell start: '<c' followed by something that can legally follow the name.
    const uint8_t c2 = b[i + 2];
    if (b[i + 1] != 'c' || (c2 != ' ' && c2 != '>' && c2 != '/' && c2 != '\t' && c2 != '\n' && c2 != '\r')) {
      ++i;
      continue;
    }

    size_t j = i + 2;
    while (j < b.size() && b[j] != '>') ++j;
    if (j >= b.size()) return false;

    // Column from r="A1" if present. Only this cell's own attribute counts, so the
    // search stops at the first r= that starts a name position.
    uint32_t col = ordinal;
    for (size_t k = i + 2; k < j; ++k) {
      const bool name_start = (k == i + 2) || b[k - 1] == ' ' || b[k - 1] == '\t' || b[k - 1] == '\n' || b[k - 1] == '\r';
      if (b[k] != 'r' || !name_start || k + 1 >= j || b[k + 1] != '=') continue;
      size_t q = k + 2;
      if (q < j && (b[q] == '"' || b[q] == '\'')) ++q;
      uint64_t letters = 0;
      size_t nletters = 0;
      while (q < j && b[q] >= 'A' && b[q] <= 'Z') {
        letters = letters * 26 + static_cast<uint64_t>(b[q] - 'A' + 1);
        ++q;
        ++nletters;
        if (letters > kMaxCols) break;
      }
      if (nletters > 0 && letters > 0 && letters <= kMaxCols) col = static_cast<uint32_t>(letters - 1);
      break;
    }
    ordinal = col + 1;

    if (col >= kMaxCols) return false;  // pathological sheet: decline rather than allocate

    // Value, if this cell has one immediately after its start tag.
    if (!starts_with(b, j + 1, "<v>")) {
      i = j + 1;
      continue;
    }
    const size_t vb = j + 4;
    size_t ve = vb;
    while (ve + 3 < b.size() && !starts_with(b, ve, "</v>")) ++ve;
    if (ve + 3 >= b.size()) return false;  // unterminated: not the XML we think it is

    out.push_back(CellValue{vb, ve, col});
    if (out.size() > 40000000u) return false;
    i = ve + 4;
  }
  return true;
}

// Zigzag over the two's-complement difference, so a delta between two int64s that
// straddle zero cannot overflow. Exact inverse in `diff_from_zz`.
uint64_t zz_from_diff(uint64_t d) { return (d << 1) ^ static_cast<uint64_t>(static_cast<int64_t>(d) >> 63); }
uint64_t diff_from_zz(uint64_t u) { return (u >> 1) ^ (0u - (u & 1u)); }

}  // namespace

bool xml_numeric_available() { return true; }

bool xml_numeric_likely(const std::span<const uint8_t> in) {
  if (in.size() < 64) return false;
  // Both markers must be there; a single scan beats two.
  bool saw_c = false;
  for (size_t i = 0; i + 2 < in.size(); ++i) {
    if (in[i] != '<') continue;
    if (in[i + 1] == 'c' && (in[i + 2] == ' ' || in[i + 2] == '>')) saw_c = true;
    else if (in[i + 1] == 'v' && in[i + 2] == '>') {
      if (saw_c) return true;
    }
  }
  return false;
}

bool xml_numeric_pack(const std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  if (!xml_numeric_likely(in)) return false;

  // An empty <v></v> already in the input would be indistinguishable from the
  // placeholders this transform creates, so the decoder could refill the wrong
  // position. Decline the whole buffer: it is rare and the alternative is a
  // guess. (The byte-compare would catch it, but not before wasting the work.)
  for (size_t i = 0; i + 6 < in.size(); ++i) {
    if (in[i] == '<' && starts_with(in, i, "<v></v>")) return false;
  }

  std::vector<CellValue> cells;
  if (!scan_cells(in, cells)) return false;

  // Accept only canonical integers; anything else stays in the skeleton as raw
  // bytes. "007", "+5", "5.0", "1.2E5" and "" all decline their own cell.
  std::vector<CellValue> taken;
  std::vector<int64_t> values;
  size_t digit_bytes = 0;
  taken.reserve(cells.size());
  values.reserve(cells.size());
  for (const CellValue& c : cells) {
    if (c.val_end <= c.val_begin) continue;  // empty <v></v> we created, or a cell with no digits
    int64_t v = 0;
    if (!parse_canonical_int(in.subspan(c.val_begin, c.val_end - c.val_begin), v)) continue;
    taken.push_back(c);
    values.push_back(v);
    digit_bytes += c.val_end - c.val_begin;
  }
  // Below this the header and the <v></v> placeholders cost more than the digits
  // they remove. The CSV transform learned the same lesson about small inputs.
  if (taken.size() < 8 || digit_bytes < 64) return false;

  std::vector<uint8_t> skeleton;
  skeleton.reserve(in.size() - digit_bytes);
  {
    size_t pos = 0;
    for (const CellValue& c : taken) {
      skeleton.insert(skeleton.end(), in.begin() + pos, in.begin() + c.val_begin);
      pos = c.val_end;
    }
    skeleton.insert(skeleton.end(), in.begin() + pos, in.end());
  }

  std::vector<uint8_t> stream;
  stream.reserve(values.size() * 2);
  {
    std::vector<int64_t> prev(kMaxCols, 0);
    std::vector<bool> seen(kMaxCols, false);
    for (size_t k = 0; k < taken.size(); ++k) {
      const uint32_t col = taken[k].col;
      const uint64_t p = seen[col] ? static_cast<uint64_t>(prev[col]) : 0u;
      const uint64_t d = static_cast<uint64_t>(values[k]) - p;
      put_varint(stream, zz_from_diff(d));
      prev[col] = values[k];
      seen[col] = true;
    }
  }

  packed.clear();
  packed.reserve(skeleton.size() + stream.size() + 16);
  packed.push_back('X');
  packed.push_back('N');
  packed.push_back('1');
  put_varint(packed, taken.size());
  put_varint(packed, skeleton.size());
  packed.insert(packed.end(), skeleton.begin(), skeleton.end());
  packed.insert(packed.end(), stream.begin(), stream.end());

  if (packed.size() >= in.size()) return false;
  return true;
}

bool xml_numeric_unpack(const std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  if (packed.size() < 8 || packed[0] != 'X' || packed[1] != 'N' || packed[2] != '1') return false;
  size_t pos = 3;
  uint64_t runs = 0;
  uint64_t skeleton_len = 0;
  if (!get_varint(packed, pos, runs)) return false;
  if (!get_varint(packed, pos, skeleton_len)) return false;
  if (skeleton_len > packed.size() - pos) return false;
  if (runs > 40000000u) return false;

  const std::span<const uint8_t> skeleton = packed.subspan(pos, skeleton_len);
  pos += skeleton_len;
  const std::span<const uint8_t> stream = packed.subspan(pos);

  // Same scanner, same column ids: the skeleton differs from the original only
  // inside the <v> tags, which the scanner does not read for column purposes.
  std::vector<CellValue> cells;
  if (!scan_cells(skeleton, cells)) return false;
  // Only the placeholders this transform created have an empty value. A cell whose
  // digits were declined — a float, "007", a string — still carries its raw bytes
  // in the skeleton and is not ours to fill; counting those as slots made the
  // decode refuse its own output, which the unit test caught before it shipped.
  std::vector<CellValue> slots;
  slots.reserve(cells.size());
  for (const CellValue& c : cells) {
    if (c.val_end == c.val_begin) slots.push_back(c);
  }
  if (slots.size() != runs) return false;

  out.clear();
  out.reserve(skeleton.size() + static_cast<size_t>(runs) * 3);

  std::vector<int64_t> prev(kMaxCols, 0);
  std::vector<bool> seen(kMaxCols, false);
  size_t spos = 0;
  size_t last = 0;
  for (const CellValue& c : slots) {
    uint64_t u = 0;
    if (!get_varint(stream, spos, u)) return false;
    const uint32_t col = c.col;
    const uint64_t p = seen[col] ? static_cast<uint64_t>(prev[col]) : 0u;
    const uint64_t v = p + diff_from_zz(u);
    prev[col] = static_cast<int64_t>(v);
    seen[col] = true;

    out.insert(out.end(), skeleton.begin() + last, skeleton.begin() + c.val_begin);
    const std::string digits = std::to_string(static_cast<int64_t>(v));
    out.insert(out.end(), digits.begin(), digits.end());
    last = c.val_end;
  }
  out.insert(out.end(), skeleton.begin() + last, skeleton.end());

  if (spos != stream.size()) return false;  // trailing bytes: not our stream
  return true;
}

}  // namespace chce
