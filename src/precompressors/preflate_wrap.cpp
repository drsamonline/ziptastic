// preflate_wrap.cpp — DEFLATE recompression with a ZIP container layer.
//
// preflate_decode() takes ONE bare DEFLATE stream, not a container. A ZIP holds
// many, interleaved with headers, so this file does the container work: walk the
// local file headers, run preflate on each stream, and record enough to splice
// the exact original back together.
//
// Every payload is verified here, on this input, before it is returned. An
// unverified transform would be a data-loss bug waiting to happen.
#include "preflate_wrap.hpp"

#include <cstring>

#include <preflate.h>

namespace chce {
namespace {

struct Stream {
  uint32_t offset = 0;   // where the DEFLATE bytes start in the original
  uint32_t comp_len = 0;
  uint32_t infl_len = 0;
  std::vector<uint8_t> diff;
  std::vector<uint8_t> inflated;
};

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(const std::vector<uint8_t>& v, size_t& pos, uint64_t& out) {
  out = 0;
  uint32_t shift = 0;
  while (pos < v.size()) {
    const uint8_t b = v[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

// Runs preflate on one stream. Returns false if it does not parse.
bool parse_stream(const uint8_t* data, uint32_t comp_len, Stream& out) {
  std::vector<unsigned char> raw(data, data + comp_len);
  std::vector<unsigned char> inflated;
  std::vector<unsigned char> diff;
  bool ok = false;
  try {
    ok = preflate_decode(inflated, diff, raw);
  } catch (...) {
    return false;
  }
  if (!ok || inflated.empty()) return false;
  out.comp_len = comp_len;
  out.infl_len = static_cast<uint32_t>(inflated.size());
  out.diff.assign(diff.begin(), diff.end());
  out.inflated.assign(inflated.begin(), inflated.end());
  return true;
}

// Walks the ZIP local file headers from the start. Bails out at the first
// structure it does not recognise, keeping whatever parsed cleanly.
std::vector<Stream> scan_zip(const std::vector<uint8_t>& in) {
  std::vector<Stream> streams;
  size_t pos = 0;
  const size_t n = in.size();
  while (pos + 30 <= n) {
    if (in[pos] != 'P' || in[pos + 1] != 'K' || in[pos + 2] != 0x03 ||
        in[pos + 3] != 0x04) {
      break;
    }
    const uint16_t method = rd16(&in[pos + 8]);
    const uint32_t comp_len = rd32(&in[pos + 18]);
    const uint16_t name_len = rd16(&in[pos + 26]);
    const uint16_t extra_len = rd16(&in[pos + 28]);
    const size_t data_off = pos + 30 + name_len + extra_len;
    if (data_off + comp_len > n) break;
    if (method == 8 && comp_len > 0) {
      Stream s;
      if (parse_stream(&in[data_off], comp_len, s)) {
        s.offset = static_cast<uint32_t>(data_off);
        streams.push_back(std::move(s));
      }
    }
    pos = data_off + comp_len;
  }
  return streams;
}


// PDF stores its content streams as DEFLATE inside `stream` ... `endstream`.
// Finding them is our own code (no licence entanglement): scan for the keyword,
// skip the end-of-line, and take everything up to `endstream`. A stream that is
// not DEFLATE simply fails to parse and is left alone.
std::vector<Stream> scan_pdf(const std::vector<uint8_t>& in) {
  std::vector<Stream> streams;
  if (in.size() < 8 || std::memcmp(in.data(), "%PDF", 4) != 0) return streams;

  const char* kStream = "stream";
  const char* kEnd = "endstream";
  size_t pos = 0;
  while (pos + 6 < in.size()) {
    const void* hit = std::memchr(in.data() + pos, 's', in.size() - pos);
    if (!hit) break;
    const size_t at = static_cast<size_t>(static_cast<const uint8_t*>(hit) - in.data());
    pos = at + 1;
    if (at + 6 > in.size() || std::memcmp(in.data() + at, kStream, 6) != 0) continue;

    size_t data = at + 6;
    if (data < in.size() && in[data] == '\r') ++data;
    if (data < in.size() && in[data] == '\n') ++data;

    // End of the data: the next "endstream".
    size_t end = data;
    while (end + 9 <= in.size()) {
      if (in[end] == 'e' && std::memcmp(in.data() + end, kEnd, 9) == 0) break;
      ++end;
    }
    if (end + 9 > in.size()) break;
    if (end <= data) { pos = end + 9; continue; }

    // The bytes just before "endstream" are an end-of-line that belongs to the
    // file's syntax, not to the stream. Feeding it to the DEFLATE parser makes
    // the stream one byte too long and the parse fails silently — which is
    // exactly what happened first.
    size_t data_end = end;
    while (data_end > data && (in[data_end - 1] == '\n' || in[data_end - 1] == '\r')) {
      --data_end;
    }
    if (data_end <= data) { pos = end + 9; continue; }

    // PDF's /FlateDecode streams are zlib-wrapped (RFC 1950): a 2-byte header
    // and a 4-byte Adler-32 around the raw DEFLATE. preflate works on the raw
    // DEFLATE only, so hand it the middle and leave the wrapper in the skeleton
    // as literal bytes. (ZIP stores raw DEFLATE, which is why this did not come
    // up until PDF.)
    size_t inner = data;
    size_t inner_len = data_end - data;
    if (inner_len > 6 && (in[inner] & 0x0F) == 8 &&
        ((static_cast<uint32_t>(in[inner]) << 8) | in[inner + 1]) % 31 == 0) {
      inner += 2;
      inner_len -= 6;  // header + Adler-32
    }

    Stream s;
    if (inner_len > 4 &&
        parse_stream(in.data() + inner, static_cast<uint32_t>(inner_len), s)) {
      s.offset = static_cast<uint32_t>(inner);
      streams.push_back(std::move(s));
    }
    pos = end + 9;
  }
  return streams;
}

bool build_payload(const std::vector<uint8_t>& in, const std::vector<Stream>& streams,
                   std::vector<uint8_t>& payload) {
  payload.clear();
  put_varint(payload, streams.size());
  for (const Stream& s : streams) {
    put_varint(payload, s.offset);
    put_varint(payload, s.comp_len);
    put_varint(payload, s.infl_len);
    put_varint(payload, s.diff.size());
  }
  for (const Stream& s : streams) payload.insert(payload.end(), s.diff.begin(), s.diff.end());
  for (const Stream& s : streams) {
    payload.insert(payload.end(), s.inflated.begin(), s.inflated.end());
  }
  // Skeleton: the original with the stream ranges cut out.
  size_t pos = 0;
  for (const Stream& s : streams) {
    if (s.offset < pos) return false;  // overlapping or unsorted: refuse
    payload.insert(payload.end(), in.begin() + pos, in.begin() + s.offset);
    pos = s.offset + s.comp_len;
  }
  payload.insert(payload.end(), in.begin() + pos, in.end());
  return true;
}

}  // namespace

bool preflate_available() { return true; }

bool preflate_unpack(const std::vector<uint8_t>& input, std::vector<uint8_t>& payload) {
  if (input.size() < 64) return false;

  std::vector<Stream> streams = scan_zip(input);
  if (streams.empty()) streams = scan_pdf(input);
  if (streams.empty()) {
    // Not a ZIP: try the whole input as one bare DEFLATE stream.
    Stream s;
    if (!parse_stream(input.data(), static_cast<uint32_t>(input.size()), s)) return false;
    s.offset = 0;
    streams.push_back(std::move(s));
  }

  if (!build_payload(input, streams, payload)) return false;

  // Verify: the payload must rebuild the input exactly. This is the only thing
  // standing between a clever idea and silent data loss.
  std::vector<uint8_t> check;
  if (!preflate_repack(payload, check)) return false;
  if (check.size() != input.size()) return false;
  if (!input.empty() && std::memcmp(check.data(), input.data(), input.size()) != 0) {
    return false;
  }
  return true;
}

bool preflate_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& original) {
  size_t pos = 0;
  uint64_t count = 0;
  if (!get_varint(payload, pos, count)) return false;
  if (count > 1000000) return false;

  struct Entry {
    uint32_t offset, comp_len, infl_len, diff_len;
  };
  std::vector<Entry> entries(static_cast<size_t>(count));
  for (auto& e : entries) {
    uint64_t a = 0, b = 0, c = 0, d = 0;
    if (!get_varint(payload, pos, a) || !get_varint(payload, pos, b) ||
        !get_varint(payload, pos, c) || !get_varint(payload, pos, d)) {
      return false;
    }
    e.offset = static_cast<uint32_t>(a);
    e.comp_len = static_cast<uint32_t>(b);
    e.infl_len = static_cast<uint32_t>(c);
    e.diff_len = static_cast<uint32_t>(d);
  }

  std::vector<std::vector<uint8_t>> diffs(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (pos + entries[i].diff_len > payload.size()) return false;
    diffs[i].assign(payload.begin() + pos, payload.begin() + pos + entries[i].diff_len);
    pos += entries[i].diff_len;
  }
  std::vector<std::vector<uint8_t>> inflated(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (pos + entries[i].infl_len > payload.size()) return false;
    inflated[i].assign(payload.begin() + pos, payload.begin() + pos + entries[i].infl_len);
    pos += entries[i].infl_len;
  }

  original.clear();
  size_t skel = pos;
  size_t orig_pos = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    if (e.offset < orig_pos) return false;
    const size_t copy = e.offset - orig_pos;
    if (skel + copy > payload.size()) return false;
    original.insert(original.end(), payload.begin() + skel, payload.begin() + skel + copy);
    skel += copy;

    std::vector<unsigned char> raw;
    const std::vector<unsigned char> unpacked(inflated[i].begin(), inflated[i].end());
    const std::vector<unsigned char> diff(diffs[i].begin(), diffs[i].end());
    bool ok = false;
    try {
      ok = preflate_reencode(raw, diff, unpacked);
    } catch (...) {
      return false;
    }
    if (!ok || raw.size() != e.comp_len) return false;
    original.insert(original.end(), raw.begin(), raw.end());
    orig_pos = e.offset + e.comp_len;
  }
  original.insert(original.end(), payload.begin() + skel, payload.end());
  return true;
}

}  // namespace chce
