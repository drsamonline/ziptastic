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
| ziptastic | `2bf5989` + lexora docs | builds clean, **21/21 suites pass** |
| heavenzip | `88ac3d6` | docs only, no implementation |
| lexora | `ad57991` | Phase-1 prototype, 502 tests local |

ziptastic `docs/` holds 20 files. Start with `BENCHMARKS.md`,
`AI_REVIEWS_SYNTHESIS.md`, `RECURSION_MEASUREMENT.md`, `LEXORA_ASSESSMENT.md`.

## 3. Measured results (through the shipped CLI, all round-trip exact)

| what | gain |
|---|---|
| Recursion: nested `.tar.gz` → `.xlsx` | **−50.7%** (369,080 → 181,791) |
| Recursion + numeric (ideal synthetic chain) | −89.4% |
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

Format **v3**: magic, version, `flags` (8 bits, **full**), `layers` (Layer 0 bits),
original_size, block_count, CRC-32.

Layer bits: `kLayerNumeric` 0x01, `kLayerNumericNested` 0x02.

## 5. Latest work this session

1. **Recursive container descent** — landed. After `precomp` inflates, the numeric
   transform re-enters on the payload; undone *before* `preflate_repack` on decode.
2. **The gate that blocked it** — `precomp_helps()` estimated the inflated payload at
   390,766 while the engine scores the same bytes at 181,633 (**2.1× pessimism**),
   declining a −50.7% transform. Fixed with a **structural signal**: if inflation
   expanded data ≥2×, take the payload. No prediction needed.
3. **Lexora assessed both ways** — see §7.

## 6. Next steps, in order

1. **XML-aware numeric transform.** The nested numeric correctly does not fire on
   XLSX because the payload is XML (`<row r="1"><c r="A1"><v>1003</v></c>`), not
   delimited text. This is the difference between −50.7% and −89.4% on that file
   class. **Highest value, best defined.**
2. **Per-block SHA-256 + index + trailer** (from lexora). We have one CRC-32 over the
   whole original — one bad byte loses a 100 MB archive. 32 bytes/block, <0.001%.
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
