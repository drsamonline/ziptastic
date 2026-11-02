// gzip_wrap.cpp — gzip framing around preflate.
//
// The DEFLATE parsing, the diff and the re-encode are preflate's, unchanged. What
// this file adds is the container: walking members, keeping each header and
// trailer verbatim, and splicing them back in the right order.
//
// Why the deflate length matters: gzip does not store it, so the end of a member's
// DEFLATE stream has to be found by parsing. preflate's streaming overload reports
// it, which is the only reason multi-member files are tractable here — with the
// vector overload we would have to guess, and the trailer plus the next member's
// header sit immediately after the stream.
#include "gzip_wrap.hpp"

#include <algorithm>
#include <cstring>

#include <preflate.h>

namespace chce {
namespace {

// Feeds preflate a byte range without copying it.
class SpanStream : public InputStream {
 public:
  SpanStream(const uint8_t* d, size_t len) : p_(d), n_(len) {}
  bool eof() const override { return pos_ >= n_; }
  size_t read(unsigned char* buf, size_t size) override {
    const size_t k = std::min(size, n_ - pos_);
    std::memcpy(buf, p_ + pos_, k);
    pos_ += k;
    return k;
  }

 private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};

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

struct Member {
  std::vector<uint8_t> header;   // magic .. end of optional fields, verbatim
  std::vector<uint8_t> trailer;  // CRC-32 | ISIZE, verbatim
  std::vector<uint8_t> diff;
  std::vector<uint8_t> inflated;
};

// Walks one member's header. Returns the offset of its DEFLATE stream, or 0 on a
// malformed header.
size_t parse_header(const std::vector<uint8_t>& in, size_t at) {
  if (at + 10 > in.size()) return 0;
  if (in[at] != 0x1f || in[at + 1] != 0x8b) return 0;
  if (in[at + 2] != 8) return 0;  // only DEFLATE is defined
  const uint8_t flg = in[at + 3];
  if ((flg & 0xE0) != 0) return 0;  // reserved bits must be zero
  size_t p = at + 10;
  if ((flg & 0x04) != 0) {  // FEXTRA
    if (p + 2 > in.size()) return 0;
    const size_t xlen = in[p] | (static_cast<size_t>(in[p + 1]) << 8);
    p += 2 + xlen;
    if (p > in.size()) return 0;
  }
  for (uint8_t flag : {uint8_t{0x08}, uint8_t{0x10}}) {  // FNAME, FCOMMENT
    if ((flg & flag) == 0) continue;
    while (p < in.size() && in[p] != 0) ++p;
    if (p >= in.size()) return 0;
    ++p;
  }
  if ((flg & 0x02) != 0) {  // FHCRC
    p += 2;
    if (p > in.size()) return 0;
  }
  return p;
}

}  // namespace

bool gzip_available() { return true; }

bool gzip_likely(const std::vector<uint8_t>& in) {
  return in.size() > 18 && in[0] == 0x1f && in[1] == 0x8b && in[2] == 8;
}

bool gzip_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  if (!gzip_likely(in)) return false;

  std::vector<Member> members;
  size_t at = 0;
  while (at < in.size()) {
    const size_t data = parse_header(in, at);
    if (data == 0) break;
    if (members.size() > 1000000u) return false;

    Member m;
    m.header.assign(in.begin() + at, in.begin() + data);

    std::vector<unsigned char> inflated, diff;
    uint64_t deflate_size = 0;
    bool ok = false;
    try {
      SpanStream stream(in.data() + data, in.size() - data);
      ok = preflate_decode(inflated, diff, deflate_size, stream, []() {}, 1);
    } catch (...) {
      return false;
    }
    if (!ok || deflate_size == 0) return false;
    const size_t trailer_at = data + static_cast<size_t>(deflate_size);
    if (trailer_at + 8 > in.size()) return false;

    m.inflated.assign(inflated.begin(), inflated.end());
    m.diff.assign(diff.begin(), diff.end());
    m.trailer.assign(in.begin() + trailer_at, in.begin() + trailer_at + 8);
    members.push_back(std::move(m));
    at = trailer_at + 8;
  }
  // Anything left over means we did not understand the file: decline rather than
  // transform part of it.
  if (members.empty() || at != in.size()) return false;

  payload.clear();
  payload.push_back('G');
  payload.push_back('Z');
  payload.push_back('0');
  payload.push_back('1');
  put_varint(payload, members.size());
  for (const Member& m : members) {
    put_varint(payload, m.header.size());
    put_varint(payload, m.diff.size());
    put_varint(payload, m.inflated.size());
  }
  for (const Member& m : members) {
    payload.insert(payload.end(), m.header.begin(), m.header.end());
  }
  for (const Member& m : members) {
    payload.insert(payload.end(), m.diff.begin(), m.diff.end());
  }
  for (const Member& m : members) {
    payload.insert(payload.end(), m.inflated.begin(), m.inflated.end());
  }
  for (const Member& m : members) {
    payload.insert(payload.end(), m.trailer.begin(), m.trailer.end());
  }

  // Verified here, on this input, before the caller ever sees it.
  std::vector<uint8_t> check;
  if (!gzip_repack(payload, check)) return false;
  if (check.size() != in.size()) return false;
  if (std::memcmp(check.data(), in.data(), in.size()) != 0) return false;
  return true;
}

bool gzip_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  if (payload.size() < 6 || payload[0] != 'G' || payload[1] != 'Z' ||
      payload[2] != '0' || payload[3] != '1') {
    return false;
  }
  size_t pos = 4;
  uint64_t count = 0;
  if (!get_varint(payload, pos, count)) return false;
  if (count == 0 || count > 1000000u) return false;

  struct Entry {
    size_t header_len, diff_len, infl_len;
  };
  std::vector<Entry> entries(static_cast<size_t>(count));
  size_t total_hdr = 0, total_diff = 0, total_infl = 0;
  for (auto& e : entries) {
    uint64_t a = 0, b = 0, c = 0;
    if (!get_varint(payload, pos, a)) return false;
    if (!get_varint(payload, pos, b)) return false;
    if (!get_varint(payload, pos, c)) return false;
    e.header_len = static_cast<size_t>(a);
    e.diff_len = static_cast<size_t>(b);
    e.infl_len = static_cast<size_t>(c);
    total_hdr += e.header_len;
    total_diff += e.diff_len;
    total_infl += e.infl_len;
  }
  if (pos + total_hdr + total_diff + total_infl + count * 8 != payload.size()) return false;

  size_t hdr_at = pos;
  size_t diff_at = hdr_at + total_hdr;
  size_t infl_at = diff_at + total_diff;
  size_t tr_at = infl_at + total_infl;

  out.clear();
  out.reserve(total_hdr + total_diff + total_infl + count * 8);
  for (const Entry& e : entries) {
    std::vector<unsigned char> raw;
    const std::vector<unsigned char> inflated(payload.begin() + infl_at,
                                              payload.begin() + infl_at + e.infl_len);
    const std::vector<unsigned char> diff(payload.begin() + diff_at,
                                          payload.begin() + diff_at + e.diff_len);
    bool ok = false;
    try {
      ok = preflate_reencode(raw, diff, inflated);
    } catch (...) {
      return false;
    }
    if (!ok) return false;

    out.insert(out.end(), payload.begin() + hdr_at, payload.begin() + hdr_at + e.header_len);
    out.insert(out.end(), raw.begin(), raw.end());
    out.insert(out.end(), payload.begin() + tr_at, payload.begin() + tr_at + 8);

    hdr_at += e.header_len;
    diff_at += e.diff_len;
    infl_at += e.infl_len;
    tr_at += 8;
  }
  return true;
}

}  // namespace chce
