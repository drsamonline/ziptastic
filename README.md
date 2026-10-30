# ziptastic

The native engine of **CHCE** (Chimera Hybrid Compression Engine): one
self-contained binary that composes preprocessors, a dual-resolution match
finder, a joint optimal parser and per-block entropy backends into a single
stream format. No external processes, no network, no model files.

**Status: Phase 1 complete** — the container, the block loop, and the Tier-1
LZMA2 fallback. Every input is also encoded as raw LZMA2 and the smaller
stream wins, so output is never worse than `xz -9` (measured: −0.73% over the
whole Silesia corpus, every file individually smaller). The CHCE-native
backends that are meant to win by more start at Phase 4.

## What it is meant to do

1. Beat `xz -9` by **≥3%** on Deflate-heavy archives (ZIP, PDF, PNG, nightly
   builds).
2. **Never be worse than `xz -9`** by more than 0.5% on any input, guaranteed
   by a per-block LZMA2 fallback.
3. With `--max-ratio`, approach the `paq8px` ratio class on general data.

The differentiator is not a new compressor. It is the composition: one parser
pricing fine matches, coarse (long-range) matches and semantic tokens with one
cost model, a per-block mode selector that re-optimises the parse for the mode
it picked, and a hard LZMA2 fallback under all of it.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: CMake ≥ 3.20 and a C++20 compiler. Nothing else — liblzma is
built from the vendored xz sources by the same CMake run, and the test suite
brings its own harness, so there is no gtest to fetch and nothing to install.

## Use

```bash
ziptastic -9 corpus/archive.zip > archive.chce   # compress
ziptastic -d archive.chce archive.zip            # decompress
cat file | ziptastic | ziptastic -d              # pipe
ziptastic bench corpus/                          # ratio + speed table
ziptastic --help
```

| option | meaning |
|---|---|
| `-9` | maximum effort preset (no CM backend) |
| `--no-lzma2` | disable the LZMA2 fallback (removes the Tier-1 guarantee) |
| `--max-ratio` | allow the CM backend — slowest, smallest (Phase 10) |
| `-b N` | block size in KiB (default 256) |
| `-o FILE` | output file |
| `-f` | overwrite an existing output |
| `-v` | per-run statistics on stderr |

## Phase plan

Phases run in order and each one's exit tests must pass before the next
begins. See `prompt` (the build instructions) and `docs/BENCHMARKS.md` for
measured results.

| Phase | Deliverable | Status |
|---|---|---|
| 0 | CMake skeleton, CLI, RAW blocks, header validation | **done** |
| 1 | LZMA2 fallback — the "never worse than xz" guarantee | **done** |
| 2 | Fine (binary-tree) match finder | next |
| 3 | Joint optimal parser | |
| 4 | Context-modelled rANS backend | |
| 5 | Adaptive frequency tables | |
| 6 | Coarse (SREP-style) long-range index | |
| 7 | Static + adaptive Huffman | |
| 8 | Semantic precompression (Preflate) | |
| 9 | BWT, Brotli, Zstd, LZ4 backends | |
| 10 | CM backend (`--max-ratio`) | |
| 11 | PPMd backend | |
| 12 | Speed/ratio optimisation | |

## Layout

```
include/chimera/   public headers (format, block, …)
src/engine/        compressor, decompressor, block writer, (later) match
                   finder, parser, mode selector
src/codecs/        thin wrappers around vendored codecs (liblzma so far)
src/entropy/       rANS and Huffman backends
src/precompressors/ DEFLATE / text / numeric pre-filters
src/dedup/         coarse block-hash index
tests/             unit, integration, benchmark, fixtures
docs/              FORMAT.md (bitstream spec), BENCHMARKS.md (results log)
vendor/hcs/        vendored upstream sources — read-only
```

`vendor/hcs/` is a mirror and is never modified. Anything that needs a change
gets a wrapper under `src/`.

## License

MIT — see `LICENSE`. Only permissively licensed sources are ever linked in;
GPL/LGPL tools (paq8px, upx, advancecomp, packJPG, lzo) are excluded from this
repository entirely.
