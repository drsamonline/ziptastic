// numeric_wrap.cpp — integer column transform.
//
// Payload:
//   "N1" | u8 delimiter | varint ncols | u8 has_header | u8 newline_is_crlf
//   varint header_len | header bytes                      (when has_header)
//   per column: u8 kind, varint len, bytes
//     kind 0 = raw text cells joined by the delimiter
//     kind 1 = zigzag varint deltas, first value absolute
#include "numeric_wrap.hpp"

#include <cstring>
#include <string>

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

uint64_t zigzag(int64_t x) { return (static_cast<uint64_t>(x) << 1) ^ static_cast<uint64_t>(x >> 63); }
int64_t unzigzag(uint64_t u) { return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1); }

// Strict canonical integer: optional '-', then digits, no leading zero unless the
// value is exactly "0". Returns false for anything that would not re-serialise
// identically, which is what keeps the round trip exact.
bool parse_canonical_int(const std::string& s, int64_t& out) {
  if (s.empty() || s.size() > 19) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '-') {
    neg = true;
    i = 1;
    if (s.size() == 1) return false;
  }
  if (s[i] == '0' && s.size() - i > 1) return false;  // leading zero
  int64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    if (v > (INT64_MAX - 9) / 10) return false;
    v = v * 10 + (s[i] - '0');
  }
  out = neg ? -v : v;
  return true;
}

std::vector<std::string> split(const std::string& line, char d) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t p = line.find(d, start);
    if (p == std::string::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

}  // namespace

bool numeric_available() { return true; }

bool numeric_likely(std::span<const uint8_t> in) {
  if (in.size() < 8) return false;
  // Decline anything with a quote: RFC 4180 quoting is real work and is not
  // handled in v1. Cheaper to decline early than to fail the byte-compare.
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '"') return false;
  }
  return true;
}

bool numeric_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  if (!numeric_likely(in)) return false;

  const std::string text(reinterpret_cast<const char*>(in.data()), in.size());
  if (text.find('\0') != std::string::npos) return false;

  // Split into rows, remembering the line ending so it can be reproduced.
  std::vector<std::string> rows;
  bool crlf = false;
  {
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        std::string line = text.substr(start, i - start);
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
          crlf = true;
        }
        rows.push_back(line);
        start = i + 1;
      }
    }
    if (start < text.size()) rows.push_back(text.substr(start));
  }
  if (rows.size() < 3) return false;  // too small for the header cost to pay

  // Sniff the delimiter: all rows must split to the same count >= 2.
  char delim = 0;
  size_t ncols = 0;
  for (char d : {',', '\t', ';', '|'}) {
    const size_t n = split(rows[0], d).size();
    if (n < 2) continue;
    bool ok = true;
    for (const std::string& r : rows) {
      if (split(r, d).size() != n) { ok = false; break; }
    }
    if (ok) { delim = d; ncols = n; break; }
  }
  if (delim == 0 || ncols < 2 || ncols > 4096) return false;

  // Header row: row 0 fails to parse as integers in at least one column while the
  // rest parse. Without this the transform would fire only on headerless data,
  // which is not what real CSVs look like.
  std::vector<std::vector<std::string>> cells;
  for (const std::string& r : rows) cells.push_back(split(r, delim));

  bool has_header = false;
  {
    size_t cols_numeric_without_header = 0;
    for (size_t c = 0; c < ncols; ++c) {
      bool all_int = true;
      for (size_t r = 1; r < cells.size() && all_int; ++r) {
        int64_t v = 0;
        if (!parse_canonical_int(cells[r][c], v)) all_int = false;
      }
      if (all_int) ++cols_numeric_without_header;
    }
    if (cols_numeric_without_header > 0) {
      // Does row 0 break that promise?
      bool row0_breaks = false;
      for (size_t c = 0; c < ncols && !row0_breaks; ++c) {
        int64_t v = 0;
        if (!parse_canonical_int(cells[0][c], v)) row0_breaks = true;
      }
      has_header = row0_breaks;
    }
  }
  const size_t first = has_header ? 1 : 0;

  // Per column: try the transform, keep it only if it beats the raw text.
  struct Col {
    uint8_t kind = 0;
    std::vector<uint8_t> data;
  };
  std::vector<Col> cols(ncols);
  size_t transformed = 0;
  for (size_t c = 0; c < ncols; ++c) {
    std::vector<int64_t> vals;
    bool all_int = true;
    for (size_t r = first; r < cells.size() && all_int; ++r) {
      int64_t v = 0;
      if (!parse_canonical_int(cells[r][c], v)) all_int = false;
      else vals.push_back(v);
    }

    std::string raw;
    for (size_t r = first; r < cells.size(); ++r) {
      if (r != first) raw.push_back(delim);
      raw += cells[r][c];
    }

    if (all_int && vals.size() == cells.size() - first && !vals.empty()) {
      std::vector<uint8_t> enc;
      int64_t prev = 0;
      for (size_t i = 0; i < vals.size(); ++i) {
        const int64_t d = (i == 0) ? vals[0] : vals[i] - prev;  // first is absolute
        prev = vals[i];
        put_varint(enc, zigzag(d));
      }
      if (enc.size() < raw.size()) {
        cols[c].kind = 1;
        cols[c].data = std::move(enc);
        ++transformed;
        continue;
      }
    }
    cols[c].kind = 0;
    cols[c].data.assign(raw.begin(), raw.end());
  }
  if (transformed == 0) return false;

  packed.clear();
  packed.push_back('N');
  packed.push_back('1');
  packed.push_back(static_cast<uint8_t>(delim));
  put_varint(packed, ncols);
  packed.push_back(has_header ? 1 : 0);
  packed.push_back(crlf ? 1 : 0);
  if (has_header) {
    put_varint(packed, rows[0].size());
    packed.insert(packed.end(), rows[0].begin(), rows[0].end());
  }
  for (const Col& c : cols) {
    packed.push_back(c.kind);
    put_varint(packed, c.data.size());
    packed.insert(packed.end(), c.data.begin(), c.data.end());
  }
  if (packed.size() >= in.size()) return false;
  return true;
}

bool numeric_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  if (packed.size() < 6 || packed[0] != 'N' || packed[1] != '1') return false;
  size_t pos = 2;
  const char delim = static_cast<char>(packed[pos++]);
  uint64_t ncols = 0;
  if (!get_varint(packed, pos, ncols) || ncols < 2 || ncols > 4096) return false;
  if (pos + 2 > packed.size()) return false;
  const bool has_header = packed[pos++] != 0;
  const bool crlf = packed[pos++] != 0;

  std::string header;
  if (has_header) {
    uint64_t n = 0;
    if (!get_varint(packed, pos, n)) return false;
    if (pos + n > packed.size()) return false;
    header.assign(reinterpret_cast<const char*>(packed.data() + pos), n);
    pos += n;
  }

  struct Col { uint8_t kind; std::span<const uint8_t> data; };
  std::vector<Col> cols(ncols);
  for (auto& c : cols) {
    if (pos >= packed.size()) return false;
    c.kind = packed[pos++];
    uint64_t n = 0;
    if (!get_varint(packed, pos, n)) return false;
    if (pos + n > packed.size()) return false;
    c.data = packed.subspan(pos, n);
    pos += n;
  }

  // Rebuild: the number of data rows is implied by any kind-1 column, or by the
  // row count in a kind-0 column.
  size_t nrows = 0;
  for (const Col& c : cols) {
    if (c.kind == 1) {
      size_t p = 0;
      size_t count = 0;
      while (p < c.data.size()) {
        uint64_t dummy = 0;
        if (!get_varint(c.data, p, dummy)) return false;
        ++count;
      }
      nrows = count;
      break;
    }
  }
  if (nrows == 0) return false;

  std::vector<std::vector<std::string>> cells(nrows, std::vector<std::string>(ncols));
  for (size_t c = 0; c < ncols; ++c) {
    if (cols[c].kind == 1) {
      size_t p = 0;
      int64_t prev = 0;
      for (size_t r = 0; r < nrows; ++r) {
        uint64_t u = 0;
        if (!get_varint(cols[c].data, p, u)) return false;
        const int64_t d = unzigzag(u);
        const int64_t v = (r == 0) ? d : prev + d;
        prev = v;
        cells[r][c] = std::to_string(v);
      }
    } else {
      const std::string txt(reinterpret_cast<const char*>(cols[c].data.data()),
                            cols[c].data.size());
      const std::vector<std::string> parts = split(txt, delim);
      if (parts.size() != nrows) return false;
      for (size_t r = 0; r < nrows; ++r) cells[r][c] = parts[r];
    }
  }

  const char* eol = crlf ? "\r\n" : "\n";
  out.clear();
  if (has_header) {
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), eol, eol + std::strlen(eol));
  }
  for (size_t r = 0; r < nrows; ++r) {
    for (size_t c = 0; c < ncols; ++c) {
      if (c) out.push_back(static_cast<uint8_t>(delim));
      out.insert(out.end(), cells[r][c].begin(), cells[r][c].end());
    }
    out.insert(out.end(), eol, eol + std::strlen(eol));
  }
  return true;
}

}  // namespace chce
