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
#include <string>
#include <unordered_map>

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
// Finds ZIP local file headers ANYWHERE in the buffer, not only at offset 0.
//
// Requiring offset 0 made two whole file classes invisible: an SFX archive is an
// .exe stub followed by a ZIP, and a `.tar.gz` holding a `.xlsx` inflates to a tar
// whose member data starts 512 bytes in. Both are containers we can parse, and
// both were being handed to the backend as opaque bytes.
//
// A false positive costs nothing: parse_stream verifies each candidate, and the
// payload is rebuilt and byte-compared before it is returned.
// The ZIP central directory, keyed by member name.
//
// A local file header may legally carry zero sizes and put the real ones in a data
// descriptor after the data — that is what a zip written to a pipe looks like, and every
// tool does it when it cannot seek. Reading only local headers therefore sees
// `comp_len == 0` and skips the member, which is why a streamed zip got no transform at
// all while the same content written by a seeking writer got 43%. The central directory
// always carries the real sizes, so it is the authority and the local header is used only
// to find where the data starts.
struct CdEntry {
  uint32_t comp_len = 0;
  uint16_t method = 0;
};

// The end-of-central-directory record is in the last 64 KiB plus its own 22 bytes, or it
// is not there: its comment field cannot be longer than 64 KiB.
bool find_eocd(const std::vector<uint8_t>& in, size_t& at) {
  const size_t n = in.size();
  if (n < 22) return false;
  const size_t lowest = n > (64u << 10) + 22 ? n - (64u << 10) - 22 : 0;
  for (size_t pos = n - 22;; --pos) {
    if (in[pos] == 'P' && in[pos + 1] == 'K' && in[pos + 2] == 0x05 && in[pos + 3] == 0x06) {
      at = pos;
      return true;
    }
    if (pos == lowest) return false;
  }
}

// Reads every entry's name, method and compressed size. The directory ends exactly where
// the end record begins, which is what locates it without knowing where the archive
// itself starts — the point when the zip is a member of something else.
bool read_central_directory(const std::vector<uint8_t>& in,
                            std::unordered_map<std::string, CdEntry>& out) {
  size_t eocd = 0;
  if (!find_eocd(in, eocd)) return false;
  const uint16_t entries = rd16(&in[eocd + 10]);
  const uint32_t cd_size = rd32(&in[eocd + 12]);
  // 0xFFFF entries means zip64, whose sizes are somewhere else entirely. Leave it to the
  // local-header path rather than half-parse it.
  if (entries == 0xFFFFu || cd_size == 0 || cd_size > eocd) return false;

  size_t pos = eocd - cd_size;
  for (uint16_t i = 0; i < entries; ++i) {
    if (pos + 46 > in.size() || std::memcmp(&in[pos], "PK\x01\x02", 4) != 0) break;
    const uint16_t method = rd16(&in[pos + 10]);
    const uint32_t comp_len = rd32(&in[pos + 20]);
    const uint16_t name_len = rd16(&in[pos + 28]);
    const uint16_t extra_len = rd16(&in[pos + 30]);
    const uint16_t comment_len = rd16(&in[pos + 32]);
    if (pos + 46 + name_len > in.size()) break;
    out.emplace(std::string(reinterpret_cast<const char*>(&in[pos + 46]), name_len),
                CdEntry{comp_len, method});
    pos += 46u + name_len + extra_len + comment_len;
  }
  return !out.empty();
}

// Finds ZIP local file headers ANYWHERE in the buffer, not only at offset 0.
//
// Requiring offset 0 made two whole file classes invisible: an SFX archive is an
// .exe stub followed by a ZIP, and a `.tar.gz` holding a `.xlsx` inflates to a tar
// whose member data starts 512 bytes in. Both are containers we can parse, and
// both were being handed to the backend as opaque bytes.
//
// A false positive costs nothing: parse_stream verifies each candidate, and the
// payload is rebuilt and byte-compared before it is returned.
std::vector<Stream> scan_zip(const std::vector<uint8_t>& in) {
  std::unordered_map<std::string, CdEntry> cd;
  const bool have_cd = read_central_directory(in, cd);

  std::vector<Stream> streams;
  size_t pos = 0;
  const size_t n = in.size();
  while (pos + 30 <= n) {
    const void* hit = std::memchr(in.data() + pos, 'P', n - pos);
    if (hit == nullptr) break;
    pos = static_cast<size_t>(static_cast<const uint8_t*>(hit) - in.data());
    if (pos + 4 > n) break;
    if (in[pos] != 'P' || in[pos + 1] != 'K' || in[pos + 2] != 0x03 ||
        in[pos + 3] != 0x04) {
      ++pos;
      continue;
    }
    if (streams.size() > 100000u) break;
    const uint16_t local_method = rd16(&in[pos + 8]);
    const uint32_t local_comp_len = rd32(&in[pos + 18]);
    const uint16_t name_len = rd16(&in[pos + 26]);
    const uint16_t extra_len = rd16(&in[pos + 28]);
    const size_t data_off = pos + 30 + name_len + extra_len;

    uint16_t method = local_method;
    uint32_t comp_len = local_comp_len;
    if (have_cd && pos + 30 + name_len <= n) {
      const std::string name(reinterpret_cast<const char*>(&in[pos + 30]), name_len);
      const auto it = cd.find(name);
      if (it != cd.end()) {
        method = it->second.method;
        comp_len = it->second.comp_len;
      }
    }

    // A member whose size is not in its local header, or that runs off the end of the
    // buffer, means "keep looking" — not "stop". The old `break` here threw away every
    // later member, including the ones that would have parsed: one stray `PK\x03\x04` in
    // a tar header or inside compressed data was enough to lose the whole archive.
    if (comp_len == 0 || data_off > n || comp_len > n - data_off) {
      ++pos;
      continue;
    }
    if (method == 8) {
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
