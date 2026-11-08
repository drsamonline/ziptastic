// brotli_wrap.cpp — brotli inflation with the producing settings recovered by trial.
//
// The whole file is a search with a verifier: decode, then try the settings a stream like this
// plausibly came from, and keep the first that re-encodes to exactly the bytes we were given.
// Nothing of the original is stored, so the verifier is the only thing standing between a wrong
// guess and wrong data — which is why it runs before the payload is returned and again when the
// payload is read back.
#include "brotli_wrap.hpp"

#include <cstring>

#include <brotli/decode.h>
#include <brotli/encode.h>

namespace chce {
namespace {

constexpr uint8_t kPayloadMagic[4] = {'B', 'R', '0', '1'};

// A ceiling on what a brotli stream may inflate to, as a function of the input rather than a flat
// 4 GiB. Measured 2026-11-08: a 38,082-byte stream that inflates to 200 MB drove the compressor to
// 810 MB of RSS, and the old flat cap let a ~130 KB file ask for 4 GiB. The policy is the decoder's
// (`true_size * 4096`, with a floor) so that a legitimately compressible file still passes and a
// crafted one is declined instead of allocated.
uint64_t max_inflated(size_t input_size) {
  const uint64_t by_ratio = static_cast<uint64_t>(input_size) * 4096;
  const uint64_t floor_cap = 256ull << 20;  // 256 MiB: generous for real content, bounded for a bomb
  return by_ratio > floor_cap ? by_ratio : floor_cap;
}

// Smallest input worth trying. Below this the decode attempt costs more than the transform could
// save, and the container framing dominates anyway.
constexpr size_t kMinInput = 1024;

void put_varint(std::vector<uint8_t>& v, uint64_t x) {
  while (x >= 0x80) {
    v.push_back(static_cast<uint8_t>(x) | 0x80u);
    x >>= 7;
  }
  v.push_back(static_cast<uint8_t>(x));
}

bool get_varint(const std::vector<uint8_t>& v, size_t& pos, uint64_t& out) {
  out = 0;
  unsigned shift = 0;
  while (pos < v.size()) {
    const uint8_t b = v[pos++];
    if (shift > 63) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

bool decode(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  const uint64_t cap = max_inflated(in.size());
  BrotliDecoderState* st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
  if (st == nullptr) return false;
  const uint8_t* next_in = in.data();
  size_t avail_in = in.size();
  uint8_t buf[1 << 16];
  bool ok = false;
  for (;;) {
    uint8_t* next_out = buf;
    size_t avail_out = sizeof(buf);
    const BrotliDecoderResult r =
        BrotliDecoderDecompressStream(st, &avail_in, &next_in, &avail_out, &next_out, nullptr);
    out.insert(out.end(), buf, next_out);
    if (out.size() > cap) break;
    if (r == BROTLI_DECODER_RESULT_SUCCESS) {
      // The whole input must have been consumed: a stream followed by other bytes is not a bare
      // brotli stream, and treating it as one would silently drop the tail.
      ok = avail_in == 0;
      break;
    }
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;
    break;  // NEEDS_MORE_INPUT or ERROR: not a brotli stream, or not a complete one
  }
  BrotliDecoderDestroyInstance(st);
  return ok && !out.empty();
}

bool encode(const std::vector<uint8_t>& raw, uint8_t quality, uint8_t mode, uint8_t lgwin,
            std::vector<uint8_t>& out) {
  BrotliEncoderState* st = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
  if (st == nullptr) return false;
  bool ok = BrotliEncoderSetParameter(st, BROTLI_PARAM_QUALITY, quality) == BROTLI_TRUE &&
            BrotliEncoderSetParameter(st, BROTLI_PARAM_MODE, mode) == BROTLI_TRUE &&
            BrotliEncoderSetParameter(st, BROTLI_PARAM_LGWIN, lgwin) == BROTLI_TRUE;
  bool finished = false;
  if (ok) {
    const uint8_t* next_in = raw.data();
    size_t avail_in = raw.size();
    uint8_t buf[1 << 16];
    for (;;) {
      uint8_t* next_out = buf;
      size_t avail_out = sizeof(buf);
      if (BrotliEncoderCompressStream(st, BROTLI_OPERATION_FINISH, &avail_in, &next_in,
                                      &avail_out, &next_out, nullptr) != BROTLI_TRUE) {
        ok = false;
        break;
      }
      out.insert(out.end(), buf, next_out);
      if (BrotliEncoderIsFinished(st) == BROTLI_TRUE) {
        finished = true;
        break;
      }
      if (avail_out != 0 && avail_in == 0 && BrotliEncoderHasMoreOutput(st) != BROTLI_TRUE) break;
    }
    // A stream that never reached FINISH is a failure, not a short answer. The re-encode verifier
    // would catch it later and decline the transform, so this fails safe either way — but it now
    // says so here, where the cause is known, instead of leaving a truncated buffer to be diagnosed
    // by a mismatch three functions away.
    if (!finished) ok = false;
  }
  BrotliEncoderDestroyInstance(st);
  return ok;
}

// The settings worth trying, in the order that finds real streams fastest. Quality 11 is what
// anything aiming at size uses; 22 is brotli's default window for it.
struct Candidate {
  uint8_t quality;
  uint8_t mode;
  uint8_t lgwin;
};

constexpr Candidate kCandidates[] = {
    {11, BROTLI_MODE_GENERIC, 22}, {11, BROTLI_MODE_TEXT, 22}, {11, BROTLI_MODE_GENERIC, 24},
    {9, BROTLI_MODE_GENERIC, 22},  {5, BROTLI_MODE_GENERIC, 22}, {1, BROTLI_MODE_GENERIC, 22},
};

}  // namespace

bool brotli_available() { return true; }

bool brotli_unpack(const std::vector<uint8_t>& in, std::vector<uint8_t>& payload) {
  if (in.size() < kMinInput) return false;

  std::vector<uint8_t> raw;
  if (!decode(in, raw)) return false;

  const Candidate* hit = nullptr;
  for (const Candidate& c : kCandidates) {
    std::vector<uint8_t> again;
    if (!encode(raw, c.quality, c.mode, c.lgwin, again)) continue;
    if (again == in) {
      hit = &c;
      break;
    }
  }
  if (hit == nullptr) return false;

  payload.clear();
  payload.reserve(raw.size() + 16);
  payload.insert(payload.end(), kPayloadMagic, kPayloadMagic + sizeof(kPayloadMagic));
  payload.push_back(hit->quality);
  payload.push_back(hit->mode);
  payload.push_back(hit->lgwin);
  put_varint(payload, raw.size());
  payload.insert(payload.end(), raw.begin(), raw.end());

  // And once more through the reader the decoder will use.
  std::vector<uint8_t> check;
  if (!brotli_repack(payload, check) || check != in) {
    payload.clear();
    return false;
  }
  return true;
}

bool brotli_repack(const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
  if (payload.size() < sizeof(kPayloadMagic) + 3 ||
      std::memcmp(payload.data(), kPayloadMagic, sizeof(kPayloadMagic)) != 0) {
    return false;
  }
  size_t pos = sizeof(kPayloadMagic);
  const uint8_t quality = payload[pos++];
  const uint8_t mode = payload[pos++];
  const uint8_t lgwin = payload[pos++];
  if (quality > 11 || mode > 2 || lgwin < 10 || lgwin > 24) return false;
  uint64_t inflated = 0;
  if (!get_varint(payload, pos, inflated)) return false;
  if (inflated != payload.size() - pos) return false;

  const std::vector<uint8_t> raw(payload.begin() + static_cast<long>(pos), payload.end());
  return encode(raw, quality, mode, lgwin, out);
}

}  // namespace chce
