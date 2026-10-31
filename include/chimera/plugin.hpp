// plugin.hpp — the stable C ABI a backend plugin must expose.
//
// Why a C ABI and not C++: a plugin is loaded by path at runtime, so it must not
// depend on this project's headers, compiler flags or STL version. Plain C, fixed
// widths, no exceptions across the boundary.
//
// Why plugins at all: some of the best backends on the vendored shelf are LGPL
// (bzip3, packJPG, packmp3). LGPL permits use from an MIT program *if the user
// can replace the library* — which a separate shared object satisfies exactly.
// Static linking would not. So the licence shapes the architecture here rather
// than merely constraining it.
//
// GPL backends (ARC, paq8px, lzo) are deliberately NOT supported by this ABI:
// dynamic linking does not escape the GPL, so a .so would not make them usable
// from an MIT program. Do not add one and assume the plugin boundary helps.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHCE_PLUGIN_ABI 1u

// Every plugin exports exactly this symbol.
#define CHCE_PLUGIN_ENTRY "chce_plugin_entry"

typedef struct chce_plugin {
  // Must equal CHCE_PLUGIN_ABI; the host refuses anything else.
  uint32_t abi_version;
  // Human-readable name and the plugin's own licence, both NUL-terminated and
  // owned by the plugin. The host prints the licence so a user always knows what
  // they are running.
  const char* name;
  const char* licence;

  // Upper bound on the compressed size of `n` input bytes.
  size_t (*bound)(size_t n);
  // Compress `n` bytes from `in` into `out` (capacity `out_cap`).
  // Returns the compressed size, or 0 when the plugin declines, or a negative
  // value on error.
  long (*compress)(const uint8_t* in, size_t n, uint8_t* out, size_t out_cap);
  // Decompress `n` bytes from `in` into `out`, which must produce exactly
  // `out_size` bytes. Returns 0 on success, negative on error.
  int (*decompress)(const uint8_t* in, size_t n, uint8_t* out, size_t out_size);
} chce_plugin;

typedef const chce_plugin* (*chce_plugin_entry_fn)(void);

#ifdef __cplusplus
}
#endif
