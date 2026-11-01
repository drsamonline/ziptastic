# Session handoff — ziptastic / heavenzip / lexora

Written 2026-11-01, end of session. Read this first in a new session; everything
below is measured unless marked otherwise.

---

## 1. What this is

Three MIT-or-source-available repos by Dr Sohil Momin, all one author:

- **ziptastic** — lossless compressor, C++20, single static binary. Its edge is
  **Layer 0**: parse the container, transform it into something more compressible,
  store a recipe that reproduces the original bytes exactly.
- **heavenzip** — orchestrator that spawns prebuilt binaries as separate processes,
  so it can drive GPL tools ziptastic legally cannot link. Still an idea; docs only.
- **lexora** — conservative text canonicalisation + compression, Python, PolyForm
  Noncommercial. Same idea applied to text.

ziptastic *links* (permissive-only, LGPL via plugin ABI). heavenzip *spawns* (much
higher licence ceiling). That difference is why both exist.

## 2. State at handoff

| repo | remote HEAD | status |
|---|---|---|
| ziptastic | `781ea9f` + per-block integrity | builds clean, **23/23 suites pass**, format **v4** |
| heavenzip | `88ac3d6` | docs only, no implementation |
| lexora | `ad57991` | Phase-1 prototype, 502 tests local |

ziptastic `docs/` holds 20 files. Start with `BENCHMARKS.md`,
`AI_REVIEWS_SYNTHESIS.md`, `RECURSION_MEASUREMENT.md`, `LEXORA_ASSESSMENT.md`.

## 3. Measured results (through the shipped CLI, all round-trip exact)

| what | gain |
|---|---|
| Recursion: nested `.tar.gz` → `.xlsx` | **−50.7%** (369,080 → 181,791) |
| Recursion + numeric (ideal synthetic chain) | −89.4% |
| **XML numeric on an `.xlsx`** | **−50.3%** (147,525 → 73,367) |
| XML numeric on the bare sheet XML | −51.2% (148,615 → 72,464) |
| PDF embedded DEFLATE | −79.1% |
| Integer CSV/TSV columns | −77.4% |
| LZ4 payload extraction | −28.8% to −58.2% (measured, **not integrated**) |
| GIF LZW → indices | −29.5% |
| ZIP/DEFLATE (preflate) | −28.8% vs `xz -9` |
| JPEG (brunsli) | −19.4% |
| base64 → binary | −4.5% dense / −71.8% structured (**not integrated**) |
| Silesia corpus, 10 files | −11.83% vs `xz -9` |

## 4. Architecture

```
Layer 0: numeric(CSV) → JPEG → GIF → precomp → [recursion] → BCJ → dedup
Layer 1: 4 MiB blocks
Layer 2: per-block best-of-backends (rANS-context, Huffman, libbsc BWT,
         LZMA2, PPMd, CM/zpaq, plugin backends)
Layer 3: whole-stream CM for --max-ratio
```

Format **v4**: magic, version, `flags` (8 bits, **full**), `layers` (Layer 0 bits),
original_size, block_count, CRC-32; per block a 32-byte SHA-256; then a `ZTIX`
trailer carrying the block count and a digest of the block manifest. v3 streams
are no longer readable — a deliberate break, see `INTEGRITY.md`.

Layer bits: `kLayerNumeric` 0x01, `kLayerNumericNested` 0x02,
`kLayerXmlNumeric` 0x04, `kLayerXmlNested` 0x08.

## 5. Latest work this session

1. **Recursive container descent** — landed. After `precomp` inflates, the numeric
   transform re-enters on the payload; undone *before* `preflate_repack` on decode.
2. **The gate that blocked it** — `precomp_helps()` estimated the inflated payload at
   390,766 while the engine scores the same bytes at 181,633 (**2.1× pessimism**),
   declining a −50.7% transform. Fixed with a **structural signal**: if inflation
   expanded data ≥2×, take the payload. No prediction needed.
3. **Lexora assessed both ways** — see §7.
4. **Per-block integrity — shipped.** SHA-256 per block, verified as each block
   comes out of the decoder (both paths), plus a 37-byte trailer binding the
   ordered block set. A damaged block is now named:
   `decompression failed: a block failed its SHA-256 check (block 2 of 13)`.
   Cost: 69 bytes single-block, 32 per extra block, and **~22% of decode wall
   time** — measured, not estimated. `docs/INTEGRITY.md` has the three cheaper
   options in the order I would try them.
5. **XML-aware numeric transform — shipped.** Spreadsheet cell values are now coded
   as delta + zigzag + varint inside the `<v>` tags, per column, at the top level and
   inside the recursion. `.xlsx` went 147,525 → **73,367 (−50.3%)**, `layers = 0x08`,
   round trip exact; a bare `sheet1.xml` 148,615 → 72,464. Three defects came out of
   it, one of them invisible to the unit tests — read
   `docs/XML_NUMERIC_MEASUREMENT.md` before touching the Layer 0 chain.

## 6. Next steps, in order

1. ~~**XML-aware numeric transform.**~~ **DONE** — −50.3% on an `.xlsx`,
   `docs/XML_NUMERIC_MEASUREMENT.md`. The remaining gap on that file class is the
   **gzip** container (item 6): precomp cannot reach inside a `.tar.gz`, so the
   nested chain never starts. That, not the transform, is now the blocker on the
   nested −89.4% shape.
2. ~~**Per-block SHA-256 + index + trailer**~~ **DONE** — `docs/INTEGRITY.md`.
   Two open decisions it left behind: the digest costs ~22% of decode time (SHA-NI
   would take it to ~2%; per-block CRC-32 would be nearly free), and the offset
   index from the lexora note was **not** built — nothing reads it, and unverified
   surface area is a liability. Revisit both before the format is called stable.
3. **Validate-before-allocate in `read_file_header`** (from lexora). Refuse malformed
   containers up front instead of defending at each use; our desync bugs surfaced as
   `bad_alloc` from an unvalidated size field.
4. **Base64 transform** — proof of concept verified on all 4 layout variants; needs
   integrating as a Layer 0 stage with accept/reject.
5. **LZ4 precompression** — measured 28.8–58.2%. Note LZ4 does *not* need its encoder
   reproduced; the parse can be stored and re-emitted.
6. **gzip / multi-member gzip** — `.tar.gz` is everywhere; mostly container work on
   the existing DEFLATE path.
7. **Parallelise the try-everything selector** — free speed, no ratio cost.
8. WAV/PCM per-channel delta, then SQLite pages.
9. ECM for **raw** CD images only (~12%; cooked `.iso` has no ECC).
10. CM backend — **last**. Nobody publishes a CPU CM design at 5 MB/s.

Also outstanding: **check for replies** to the permission emails sent 2026-11-01 to
Ziganshin (SREP) and Walbeehm (DeflOpt).

## 7. Cross-repo state

- `lexora/docs/ZIPTASTIC_NOTES.md` — what lexora should take from ziptastic. Chiefly:
  **swap zstd-19 for ziptastic as its codec** (Silesia: ziptastic 23,838,035 vs
  `xz -9` 27,035,452, and xz beats zstd-19 on most text — a double-digit gain sitting
  there). Also the measured-and-lost table so it does not rebuild a word dictionary
  worth −0.38%, and a warning that CAN-R01 (encoding → UTF-8) doubles byte count and
  is a *loss* for a compressor downstream — different objectives, not a bug.
- `ziptastic/docs/LEXORA_ASSESSMENT.md` — the reverse. Three hardening ideas in
  (validate-before-allocate, per-block integrity, stated determinism); three
  canonicalisation rules out (measured negligible or harmful).

## 8. Rebuilding the environment (sandbox recycles often)

```bash
cd /scratch && mkdir -p repos && cd repos
git clone --depth 1 https://github.com/drsamonline/ziptastic.git
cd /scratch/work && pip install --quiet cmake ninja ziglang
mkdir -p toolchain && cd toolchain
ZIG=/scratch/.local/lib/python3.12/site-packages/ziglang/zig
for n in cc ar; do printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s %s "$@"\n' "$ZIG" "$n" > zig-$n; chmod +x zig-$n; done
printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s c++ "$@"\n' "$ZIG" > zig-cxx; chmod +x zig-cxx
```

Configure and build **in separate calls** — each exceeds the 300 s cap:

```bash
cd /scratch/repos/ziptastic
/scratch/.local/bin/cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/scratch/work/toolchain/zig-cc \
  -DCMAKE_CXX_COMPILER=/scratch/work/toolchain/zig-cxx \
  -DCMAKE_AR=/scratch/work/toolchain/zig-ar
```
```bash
/scratch/.local/bin/cmake --build build -j 2      # may need 2-3 calls
```
```bash
/scratch/.local/bin/ctest --test-dir build        # expect 21/21 passing
```

Vendored sources are submodules: `git submodule update --init vendor/hcs/old/<name>`.

## 9. Hard-won gotchas

- **`git push` is 403.** Commit via `GITHUB_COMMIT_MULTIPLE_FILES` in `run_program`.
- The commit path is **text-only UTF-8** and *silently skips* non-UTF-8 files
  (UC2's `SUPERMAN.CPP`/`COMOTERP.CPP` are CP1251 — transcode first). Binaries cannot
  be committed; embed test fixtures as byte arrays.
- Use `git diff-tree --name-only -z` and split on NUL. Plain `--name-only` fragments
  paths with spaces.
- **Never `--amend` in a shallow clone** — folds changes into the root commit. Reset
  onto `FETCH_HEAD` and commit cleanly.
- Cloned repos become gitlinks; register each in `.gitmodules` or a fresh clone gets
  empty directories.
- `libbz3.c` needs `-DVERSION="1.5.1"`; brotli must compile as **C**; brunsli's
  vendored copy needs `false` added at two call sites.
- Verify a transform **engaged** (read the `flags`/`layers` byte), not just that the
  output shrank. This has caught four real bugs.

## 10. Standing rules

1. **Verify before use.** Never commit a transform whose round trip has not been
   checked on that exact input. Five would-be corruptions turned into declines.
2. **Measure, don't predict.** Three predictors wrong, all costing ratio: the sample
   selector (+2.68% size), the 256 KiB mid-sample, and this session's 2.1× pessimism.
   Where a structural fact about the input exists, use it.
3. **A transform pays only if it removes redundancy the backend cannot already
   capture.** This rule predicted every negative result.
4. **Licences:** ziptastic links → permissive only (LGPL via plugin, GPL never).
   heavenzip spawns → aggregation, each tool with its own licence. SREP has **no
   licence** and can never be shipped by us.
5. **Document everything**, failures included — half of `docs/` is negative results,
   and those save the most time.
