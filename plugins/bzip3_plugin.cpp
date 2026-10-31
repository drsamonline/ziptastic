// bzip3_plugin.cpp — bzip3 as a separately loadable backend.
//
// bzip3 is LGPL-3. LGPL permits use from an MIT program *provided the user can
// replace the library*, and a separate shared object satisfies that where static
// linking would not. This file is compiled into bzip3_plugin.so, which is
// distributed under LGPL-3 alongside ziptastic's MIT core; nothing LGPL is linked
// into the main binary.
//
// Measured against libbsc's BWT on identical 4 MiB blocks: -4.35% on `mr`,
// -3.27% on `x-ray`, -1.22% on `dickens`; +6.59% on `xml`, which is why this is
// offered as a candidate and not as a replacement.
#include <cstring>

#include <libbz3.h>

#include "chimera/plugin.hpp"

namespace {

long p_compress(const uint8_t* in, size_t n, uint8_t* out, size_t out_cap) {
  if (n == 0 || n > (size_t)0x7FFFFFFF) return 0;
  if (out_cap < bz3_bound(n)) return -1;

  bz3_state* s = bz3_new(static_cast<int32_t>(n < (1u << 20) ? (1u << 20) : n));
  if (!s) return -1;
  std::memcpy(out, in, n);
  const int32_t r = bz3_encode_block(s, out, static_cast<int32_t>(n));
  bz3_free(s);
  // bz3_encode_block returns the COMPRESSED SIZE, not an error code (-1 = error).
  if (r < 0) return -1;
  if (static_cast<size_t>(r) >= n) return 0;  // no gain: decline
  return r;
}

int p_decompress(const uint8_t* in, size_t n, uint8_t* out, size_t out_size) {
  if (n == 0 || n > (size_t)0x7FFFFFFF) return -1;
  bz3_state* s = bz3_new(static_cast<int32_t>(out_size < (1u << 20) ? (1u << 20) : out_size));
  if (!s) return -1;
  std::memcpy(out, in, n);
  // (state, buffer, buffer_size, compressed_size, orig_size) — the capacity is a
  // separate argument from the original size.
  const int32_t r = bz3_decode_block(s, out, out_size, static_cast<int32_t>(n),
                                     static_cast<int32_t>(out_size));
  bz3_free(s);
  return r < 0 ? -1 : 0;
}

size_t p_bound(size_t n) { return bz3_bound(n); }

const chce_plugin kPlugin = {
    CHCE_PLUGIN_ABI,
    "bzip3",
    "LGPL-3.0",
    p_bound,
    p_compress,
    p_decompress,
};

}  // namespace

extern "C" const chce_plugin* chce_plugin_entry(void) { return &kPlugin; }
