// wav_wrap.cpp — 16-bit PCM as per-channel deltas.
//
// See the header for the measurements. The short version: the transform pays on
// tonal material (-16.5%) and barely at all on noise (-2.0%), so the caller runs
// an accept/reject trial and this file only has to be exact.
#include "wav_wrap.hpp"

#include <cstring>

#include "numeric_common.hpp"

namespace chce {
namespace {

using namespace numdetail;

// Fixed-width trailer, so it can be parsed from the end:
//   u32 stream_len | u32 head_len | u32 tail_len | u8 channels | u8 reserved | "WV01"
constexpr size_t kTrailer = 4 + 4 + 4 + 1 + 1 + 4;

uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint64_t zz_from_diff(uint64_t d) {
  return (d << 1) ^ static_cast<uint64_t>(static_cast<int64_t>(d) >> 63);
}
uint64_t diff_from_zz(uint64_t u) { return (u >> 1) ^ (0u - (u & 1u)); }

struct Layout {
  size_t data_at = 0;
  size_t data_len = 0;
  uint32_t channels = 0;
  uint32_t bytes_per_sample = 2;  // 2 = 16-bit, 3 = 24-bit, 4 = 32-bit
  bool is_float = false;          // 32-bit IEEE float, delta-coded on its bit pattern
};

bool parse(std::span<const uint8_t> in, Layout& out) {
  if (in.size() < 44) return false;
  if (std::memcmp(in.data(), "RIFF", 4) != 0) return false;
  if (std::memcmp(in.data() + 8, "WAVE", 4) != 0) return false;

  size_t p = 12;
  bool have_fmt = false;
  while (p + 8 <= in.size()) {
    const char* id = reinterpret_cast<const char*>(in.data() + p);
    const uint32_t sz = rd32(in.data() + p + 4);
    const size_t body = p + 8;
    if (body > in.size() || sz > in.size() - body) return false;

    if (std::memcmp(id, "fmt ", 4) == 0) {
      if (sz < 16) return false;
      const uint16_t format = rd16(in.data() + body);
      out.channels = rd16(in.data() + body + 2);
      const uint16_t bits = rd16(in.data() + body + 14);
      // 16/24/32-bit integer PCM, and 32-bit float. The float case is delta-coded on
      // its BIT PATTERN: adjacent float samples from smooth audio have close
      // exponents and mantissas, so the integer difference is small even though the
      // values themselves are not. Measured at -52.7% on music, against -56.7% for
      // 24-bit integer of the same signal.
      if (format == 1 && (bits == 16 || bits == 24 || bits == 32)) {
        out.is_float = false;
      } else if (format == 3 && bits == 32) {
        out.is_float = true;
      } else {
        return false;
      }
      out.bytes_per_sample = bits / 8;
      if (out.channels == 0 || out.channels > 8) return false;
      have_fmt = true;
    } else if (std::memcmp(id, "data", 4) == 0) {
      if (!have_fmt) return false;
      out.data_at = body;
      out.data_len = sz;
      const size_t frame = static_cast<size_t>(out.channels) * out.bytes_per_sample;
      // Whole frames only. A trailing odd sample would not decode back to the same
      // byte count, and "probably fine" is not the contract.
      if (out.data_len == 0 || out.data_len % frame != 0) return false;
      return true;
    }
    p = body + sz + (sz & 1);  // chunks are word-aligned
  }
  return false;
}

}  // namespace

bool wav_available() { return true; }

bool wav_likely(std::span<const uint8_t> in) {
  Layout l;
  return parse(in, l);
}

bool wav_pack(std::span<const uint8_t> in, std::vector<uint8_t>& packed) {
  Layout l;
  if (!parse(in, l)) return false;

  const size_t bps = l.bytes_per_sample;
  const size_t frame_bytes = static_cast<size_t>(l.channels) * bps;
  const size_t frames = l.data_len / frame_bytes;
  std::vector<uint8_t> stream;
  stream.reserve(l.data_len);
  {
    std::vector<int64_t> prev(l.channels, 0);
    const uint8_t* p = in.data() + l.data_at;
    for (size_t f = 0; f < frames; ++f) {
      for (uint32_t c = 0; c < l.channels; ++c) {
        uint64_t raw = 0;
        for (size_t b = 0; b < bps; ++b) raw |= static_cast<uint64_t>(p[b]) << (8 * b);
        p += bps;
        // Sign-extend for the integer formats. Float keeps its raw 32-bit pattern:
        // the sign bit is the sign bit either way, so the same code path is exact.
        int64_t s;
        if (!l.is_float && bps < 4) {
          const uint64_t sign = uint64_t{1} << (bps * 8 - 1);
          s = (raw & sign) ? static_cast<int64_t>(raw | ~((sign << 1) - 1))
                           : static_cast<int64_t>(raw);
        } else if (l.is_float) {
          s = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(raw)));
        } else {
          s = static_cast<int64_t>(raw);
        }
        const uint64_t d = static_cast<uint64_t>(s) - static_cast<uint64_t>(prev[c]);
        put_varint(stream, zz_from_diff(d));
        prev[c] = s;
      }
    }
  }
  if (stream.size() >= l.data_len) return false;  // nothing gained on the raw bytes

  const size_t tail_at = l.data_at + l.data_len;
  const uint32_t stream_len = static_cast<uint32_t>(stream.size());
  const uint32_t head_len = static_cast<uint32_t>(l.data_at);
  const uint32_t tail_len = static_cast<uint32_t>(in.size() - tail_at);

  packed.clear();
  packed.reserve(stream.size() + head_len + tail_len + kTrailer);
  packed.insert(packed.end(), stream.begin(), stream.end());
  packed.insert(packed.end(), in.begin(), in.begin() + l.data_at);
  packed.insert(packed.end(), in.begin() + tail_at, in.end());
  for (int i = 0; i < 4; ++i) packed.push_back(static_cast<uint8_t>(stream_len >> (8 * i)));
  for (int i = 0; i < 4; ++i) packed.push_back(static_cast<uint8_t>(head_len >> (8 * i)));
  for (int i = 0; i < 4; ++i) packed.push_back(static_cast<uint8_t>(tail_len >> (8 * i)));
  packed.push_back(static_cast<uint8_t>(l.channels));
  packed.push_back(static_cast<uint8_t>((l.is_float ? 0x80 : 0) | (bps & 0x0F)));
  packed.push_back('W');
  packed.push_back('V');
  packed.push_back('0');
  packed.push_back('1');
  return packed.size() < in.size();
}

bool wav_unpack(std::span<const uint8_t> packed, std::vector<uint8_t>& out) {
  if (packed.size() < kTrailer) return false;
  const size_t t = packed.size();
  if (packed[t - 4] != 'W' || packed[t - 3] != 'V' || packed[t - 2] != '0' ||
      packed[t - 1] != '1') {
    return false;
  }
  const uint32_t stream_len = rd32(packed.data() + t - kTrailer);
  const uint32_t head_len = rd32(packed.data() + t - kTrailer + 4);
  const uint32_t tail_len = rd32(packed.data() + t - kTrailer + 8);
  const uint32_t channels = packed[t - kTrailer + 12];
  const uint8_t shape = packed[t - kTrailer + 13];
  const bool is_float = (shape & 0x80) != 0;
  const size_t bps = shape & 0x0F;
  if (channels == 0 || channels > 8) return false;
  if (bps != 2 && bps != 3 && bps != 4) return false;
  if (is_float && bps != 4) return false;
  const uint64_t need = static_cast<uint64_t>(stream_len) + head_len + tail_len + kTrailer;
  if (need != packed.size()) return false;

  const std::span<const uint8_t> stream = packed.first(stream_len);
  const std::span<const uint8_t> head = packed.subspan(stream_len, head_len);
  const std::span<const uint8_t> tail = packed.subspan(stream_len + head_len, tail_len);

  // Walk the varints once to learn how many samples there are, so the output can be
  // sized before it is built.
  size_t probe = 0;
  size_t samples = 0;
  while (probe < stream.size()) {
    uint64_t u = 0;
    if (!get_varint(stream, probe, u)) return false;
    ++samples;
  }
  if (samples == 0 || samples % channels != 0) return false;

  out.clear();
  out.reserve(head.size() + samples * bps + tail.size());
  out.insert(out.end(), head.begin(), head.end());

  std::vector<int64_t> prev(channels, 0);
  size_t pos = 0;
  for (size_t k = 0; k < samples; ++k) {
    uint64_t u = 0;
    if (!get_varint(stream, pos, u)) return false;
    const uint32_t c = static_cast<uint32_t>(k % channels);
    const uint64_t v = static_cast<uint64_t>(prev[c]) + diff_from_zz(u);
    const int64_t s = static_cast<int64_t>(v);
    // Range check the integer formats; float is just a bit pattern and any 32-bit
    // value is a legal one.
    if (!is_float) {
      if (bps == 2 && (s < -32768 || s > 32767)) return false;
      if (bps == 3 && (s < -8388608 || s > 8388607)) return false;
      if (bps == 4 && (s < INT32_MIN || s > INT32_MAX)) return false;
    }
    prev[c] = s;
    const uint64_t raw = static_cast<uint64_t>(s);
    for (size_t b = 0; b < bps; ++b) out.push_back(static_cast<uint8_t>(raw >> (8 * b)));
  }
  out.insert(out.end(), tail.begin(), tail.end());
  if (pos != stream.size()) return false;
  return true;
}

}  // namespace chce
