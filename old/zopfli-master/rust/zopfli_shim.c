/*
Copyright 2026 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <stddef.h>

#include "deflate.h"
#include "gzip_container.h"
#include "lz77.h"
#include "zlib_container.h"
#include "zopfli.h"

#ifdef __cplusplus
extern "C" {
#endif

// Declarations of pure Rust exported FFI functions with the Rust_Zopfli prefix.
void Rust_ZopfliInitOptions(ZopfliOptions* options);

void Rust_ZopfliCompress(const ZopfliOptions* options, int output_type,
                         const unsigned char* in_data, size_t insize,
                         unsigned char** out, size_t* outsize);

void Rust_ZopfliGzipCompress(const ZopfliOptions* options,
                             const unsigned char* in_data, size_t insize,
                             unsigned char** out, size_t* outsize);

void Rust_ZopfliZlibCompress(const ZopfliOptions* options,
                             const unsigned char* in_data, size_t insize,
                             unsigned char** out, size_t* outsize);

void Rust_ZopfliDeflate(const ZopfliOptions* options, int btype,
                        int final_block, const unsigned char* in_data,
                        size_t insize, unsigned char* bp, unsigned char** out,
                        size_t* outsize);

void Rust_ZopfliDeflatePart(const ZopfliOptions* options, int btype,
                            int final_block, const unsigned char* in_data,
                            size_t instart, size_t inend, unsigned char* bp,
                            unsigned char** out, size_t* outsize);

double Rust_ZopfliCalculateBlockSize(const ZopfliLZ77Store* lz77, size_t lstart,
                                     size_t lend, int btype);

double Rust_ZopfliCalculateBlockSizeAutoType(const ZopfliLZ77Store* lz77,
                                             size_t lstart, size_t lend);

void Rust_ZopfliInitLZ77Store(const unsigned char* data,
                              ZopfliLZ77Store* store);

void Rust_ZopfliCleanLZ77Store(ZopfliLZ77Store* store);

// Standard C API drop-in implementations forwarding to Rust_Zopfli* symbols.

void ZopfliInitOptions(ZopfliOptions* options) {
  Rust_ZopfliInitOptions(options);
}

void ZopfliCompress(const ZopfliOptions* options, ZopfliFormat output_type,
                    const unsigned char* in, size_t insize,
                    unsigned char** out, size_t* outsize) {
  Rust_ZopfliCompress(options, (int)output_type, in, insize, out, outsize);
}

void ZopfliGzipCompress(const ZopfliOptions* options,
                        const unsigned char* in, size_t insize,
                        unsigned char** out, size_t* outsize) {
  Rust_ZopfliGzipCompress(options, in, insize, out, outsize);
}

void ZopfliZlibCompress(const ZopfliOptions* options,
                        const unsigned char* in, size_t insize,
                        unsigned char** out, size_t* outsize) {
  Rust_ZopfliZlibCompress(options, in, insize, out, outsize);
}

void ZopfliDeflate(const ZopfliOptions* options, int btype, int final_block,
                   const unsigned char* in, size_t insize,
                   unsigned char* bp, unsigned char** out, size_t* outsize) {
  Rust_ZopfliDeflate(options, btype, final_block, in, insize, bp, out, outsize);
}

void ZopfliDeflatePart(const ZopfliOptions* options, int btype, int final_block,
                       const unsigned char* in, size_t instart, size_t inend,
                       unsigned char* bp, unsigned char** out,
                       size_t* outsize) {
  Rust_ZopfliDeflatePart(options, btype, final_block, in, instart, inend, bp,
                         out, outsize);
}

double ZopfliCalculateBlockSize(const ZopfliLZ77Store* lz77,
                                size_t lstart, size_t lend, int btype) {
  return Rust_ZopfliCalculateBlockSize(lz77, lstart, lend, btype);
}

double ZopfliCalculateBlockSizeAutoType(const ZopfliLZ77Store* lz77,
                                        size_t lstart, size_t lend) {
  return Rust_ZopfliCalculateBlockSizeAutoType(lz77, lstart, lend);
}

void ZopfliInitLZ77Store(const unsigned char* data, ZopfliLZ77Store* store) {
  Rust_ZopfliInitLZ77Store(data, store);
}

void ZopfliCleanLZ77Store(ZopfliLZ77Store* store) {
  Rust_ZopfliCleanLZ77Store(store);
}

#ifdef __cplusplus
}  // extern "C"
#endif
