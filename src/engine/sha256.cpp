// sha256.cpp — FIPS 180-4 SHA-256.
//
// Written out rather than vendored: 120 lines, no licence question, no API to
// adapt, and the test vectors in tests/unit/test_integrity.cpp pin it to the
// standard. Speed is irrelevant here — this runs once per block, and a block is
// 4 MiB by default.
#include "chimera/sha256.hpp"

#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CHCE_HAVE_SHA_NI 1
#endif

namespace chce {
namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
           (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(p[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

#ifdef CHCE_HAVE_SHA_NI
// SHA-NI: the same 64 rounds, four at a time, using the CPU's SHA extensions.
//
// Measured on this machine: the scalar loop above runs at 140.8 MB/s, which is about
// 22% of decode wall time — a real cost for integrity that nobody asked to pay. The
// instruction path is 1.5-2 GB/s, which puts it near 2%. Selected at runtime, because
// a build that only ran on SHA-capable CPUs would be a worse trade than the 20%.
__attribute__((target("sha,sse4.1")))
void compress_ni(uint32_t h[8], const uint8_t* p) {
  const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

  __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[0]));
  __m128i cdgh = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&h[4]));
  tmp = _mm_shuffle_epi32(tmp, 0xB1);        // CDAB
  cdgh = _mm_shuffle_epi32(cdgh, 0x1B);      // EFGH
  __m128i abef = _mm_alignr_epi8(tmp, cdgh, 8);
  cdgh = _mm_blend_epi16(cdgh, tmp, 0xF0);
  const __m128i abef_save = abef;
  const __m128i cdgh_save = cdgh;

  __m128i w[16];
  for (int i = 0; i < 4; ++i) {
    w[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), mask);
  }
  for (int i = 4; i < 16; ++i) {
    __m128i t = _mm_sha256msg1_epu32(w[i - 4], w[i - 3]);
    t = _mm_add_epi32(t, _mm_alignr_epi8(w[i - 1], w[i - 2], 4));
    w[i] = _mm_sha256msg2_epu32(t, w[i - 1]);
  }
  for (int i = 0; i < 16; ++i) {
    __m128i m = _mm_add_epi32(w[i], _mm_loadu_si128(reinterpret_cast<const __m128i*>(&kK[4 * i])));
    cdgh = _mm_sha256rnds2_epu32(cdgh, abef, m);
    m = _mm_shuffle_epi32(m, 0x0E);
    abef = _mm_sha256rnds2_epu32(abef, cdgh, m);
  }

  abef = _mm_add_epi32(abef, abef_save);
  cdgh = _mm_add_epi32(cdgh, cdgh_save);
  tmp = _mm_shuffle_epi32(abef, 0x1B);   // FEBA
  cdgh = _mm_shuffle_epi32(cdgh, 0xB1);  // DCHG
  const __m128i out0 = _mm_blend_epi16(tmp, cdgh, 0xF0);
  const __m128i out1 = _mm_alignr_epi8(cdgh, tmp, 8);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[0]), out0);
  _mm_storeu_si128(reinterpret_cast<__m128i*>(&h[4]), out1);
}

// CPUID leaf 7, EBX bit 29. Done by hand rather than with __builtin_cpu_supports,
// which pulls in libgcc's __cpu_model/__cpu_features2 globals that this toolchain
// does not link.
bool cpu_has_sha() {
  unsigned max_leaf = 0, b = 0, c = 0, d = 0;
  __asm__ __volatile__("cpuid" : "=a"(max_leaf) : "a"(0) : "ebx", "ecx", "edx");
  if (max_leaf < 7) return false;
  __asm__ __volatile__("cpuid" : "=a"(max_leaf), "=b"(b), "=c"(c), "=d"(d)
                       : "a"(7), "c"(0));
  return (b & (1u << 29)) != 0;
}

const bool g_have_sha_ni = cpu_has_sha();

inline void compress_dispatch(uint32_t h[8], const uint8_t* p) {
  if (g_have_sha_ni) {
    compress_ni(h, p);
    return;
  }
  compress(h, p);
}
#else
inline void compress_dispatch(uint32_t h[8], const uint8_t* p) { compress(h, p); }
#endif

}  // namespace

namespace {

template <typename Compress>
Sha256Digest sha256_with(Compress compress_one, std::span<const uint8_t> data) {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  const uint64_t bitlen = static_cast<uint64_t>(data.size()) * 8u;

  size_t i = 0;
  for (; i + 64 <= data.size(); i += 64) compress_one(h, data.data() + i);

  uint8_t tail[128] = {};
  const size_t rem = data.size() - i;
  if (rem != 0) std::memcpy(tail, data.data() + i, rem);
  tail[rem] = 0x80;
  const size_t tail_len = (rem + 1 + 8 <= 64) ? 64 : 128;
  for (int b = 0; b < 8; ++b) {
    tail[tail_len - 1 - b] = static_cast<uint8_t>(bitlen >> (8 * b));
  }
  compress_one(h, tail);
  if (tail_len == 128) compress_one(h, tail + 64);

  Sha256Digest out{};
  for (int b = 0; b < 8; ++b) {
    out[b * 4] = static_cast<uint8_t>(h[b] >> 24);
    out[b * 4 + 1] = static_cast<uint8_t>(h[b] >> 16);
    out[b * 4 + 2] = static_cast<uint8_t>(h[b] >> 8);
    out[b * 4 + 3] = static_cast<uint8_t>(h[b]);
  }
  return out;
}

}  // namespace

Sha256Digest sha256(std::span<const uint8_t> data) {
  return sha256_with(compress_dispatch, data);
}

Sha256Digest sha256_scalar(std::span<const uint8_t> data) {
  return sha256_with([](uint32_t h[8], const uint8_t* p) { compress(h, p); }, data);
}

}  // namespace chce
