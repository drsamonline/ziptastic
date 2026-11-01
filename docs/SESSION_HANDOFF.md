# Session handoff — ziptastic / heavenzip

Written 2026-11-01 to resume in a fresh session. Read this first; everything below
is measured unless marked otherwise.

---

## 1. What this is

Two MIT repos by Dr Sohil Momin:

- **ziptastic** — lossless compressor, C++20, single static binary. Its edge is not
  the entropy coder but **Layer 0**: parse the container, transform it into something
  more compressible, store a recipe that reproduces the original bytes exactly.
- **heavenzip** — orchestrator that spawns prebuilt binaries as separate processes,
  so it can drive GPL tools that ziptastic legally cannot link.

The split exists because of licensing: ziptastic *links* (permissive-only, LGPL via
plugin ABI), heavenzip *spawns* (much higher licence ceiling).

## 2. State at handoff

| repo | remote HEAD | status |
|---|---|---|
| ziptastic | `2bf5989` | builds clean, **21/21 test suites pass** |
| heavenzip | `88ac3d6` | docs only, still an idea — no implementation |

ziptastic `docs/` has 18 files; read `BENCHMARKS.md`, `AI_REVIEWS_SYNTHESIS.md` and
`RECURSION_MEASUREMENT.md` for the current picture.

## 3. Measured results (all through the shipped CLI, all round-trip exact)

| what | gain |
|---|---|
| Recursion: nested `.tar.gz` → `.xlsx` (reaching the XML inside) | **−50.7%** (369,080 → 181,791) |
| Recursion + numeric (ideal, measured on a synthetic chain) | −89.4% |
| PDF embedded DEFLATE | −79.1% |
| Integer CSV/TSV columns | −77.4% |
| LZ4 payload extraction | −28.8% to −58.2% (measured, **not integrated**) |
| GIF LZW → indices | −29.5% |
| ZIP/DEFLATE (preflate) | −28.8% vs `xz -9` |
| JPEG (brunsli) | −19.4% |
| base64 → binary | −4.5% dense, −71.8% structured (measured, **not integrated**) |
| Silesia corpus, 10 files | −11.83% vs `xz -9` |

## 4. Architecture

```
Layer 0 (containers): numeric(CSV) → JPEG → GIF → precomp → [recursion] → BCJ → dedup
Layer 1: 4 MiB blocks
Layer 2: per-block best-of-backends (rANS-context, Huffman, libbsc BWT,
         LZMA2, PPMd, CM/zpaq, plugin backends)
Layer 3: whole-stream CM for --max-ratio
```

Format is **v3**: `FileHeader` = magic, version, `flags` (8 bits, **full**), `layers`
(Layer 0 bits), original_size, block_count, CRC-32.

Layer bits: `kLayerNumeric` (0x01), `kLayerNumericNested` (0x02).

## 5. What just landed (the last two commits)

Recursive container descent. After `precomp` inflates a container, the numeric
transform re-enters on the payload; on decode it is undone **before** `preflate_repack`.
Enabled by a `kLayerNumericNested` bit.

It only started working after fixing a **gate** bug: `precomp_helps()` estimated the
inflated payload at 390,766 while the real engine scores the same bytes at 181,633 —
**2.1× pessimism**, which declined a −50.7% transform. Fix: a **structural signal**
(if inflation expanded the data ≥2×, take the payload) instead of a prediction.

## 6. Next steps, in order

1. **XML-aware numeric transform.** The nested numeric correctly does *not* fire on
   XLSX because the payload is XML (`<row r="1"><c r="A1"><v>1003</v></c>`), not
   delimited text. Recognising cell values is the difference between −50.7% and
   −89.4% on that file class. **Highest value, well-defined.**
2. **Base64 transform.** Proof of concept written and verified (all 4 layout variants
   exact); needs integrating as a Layer 0 stage with accept/reject.
3. **LZ4 precompression.** Measured 28.8–58.2%; frame parser + parse-storage +
   `kLayerLz4`. Note LZ4 does *not* need its encoder reproduced — the parse can be
   stored and re-emitted.
4. **gzip / multi-member gzip.** `.tar.gz` is everywhere; mostly container work on
   the existing DEFLATE path.
5. **Parallelise the try-everything selector.** Transforms are independent and run
   serially. Free speed, no ratio cost.
6. **WAV/PCM per-channel delta** (~45–60% claimed), then SQLite pages.
7. ECM for **raw** CD images only (~12%; cooked `.iso` has no ECC — do not overstate
   this as I did).
8. CM backend — **last**. Nobody publishes a CPU CM design at 5 MB/s; LPAQ runs
   ~0.45 MB/s. Byte-aligned CM with a vectorised mixer (FWHT) is the only viable
   shape.

## 7. Rebuilding the environment (the sandbox recycles often)

```bash
cd /scratch && mkdir -p repos && cd repos
git clone --depth 1 https://github.com/drsamonline/ziptastic.git
cd /scratch/work && pip install --quiet cmake ninja ziglang
mkdir -p toolchain && cd toolchain
ZIG=/scratch/.local/lib/python3.12/site-packages/ziglang/zig
for n in cc ar; do printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s %s "$@"\n' "$ZIG" "$n" > zig-$n; chmod +x zig-$n; done
printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s c++ "$@"\n' "$ZIG" > zig-cxx; chmod +x zig-cxx
cd /scratch/repos/ziptastic
/scratch/.local/bin/cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/scratch/work/toolchain/zig-cc \
  -DCMAKE_CXX_COMPILER=/scratch/work/toolchain/zig-cxx \
  -DCMAKE_AR=/scratch/work/toolchain/zig-ar
/scratch/.local/bin/cmake --build build -j 2      # needs 2-3 calls; it exceeds 300s
```

Configure and build both routinely exceed the 300 s command cap — **split them**.
`git submodule update --init vendor/hcs/old/<name>` materialises a vendored source.

## 8. Hard-won gotchas

- **`git push` is 403** from the sandbox. Commits go through
  `GITHUB_COMMIT_MULTIPLE_FILES` in `run_program`.
- **The commit path is text-only and UTF-8.** Non-UTF-8 files are *silently skipped*
  (UC2's `SUPERMAN.CPP`/`COMOTERP.CPP` are CP1251 — transcode first). Binary files
  are skipped too, so test fixtures must be embedded as byte arrays.
- **Use `git diff-tree --name-only -z`** and split on NUL. Plain `--name-only`
  escapes paths with spaces, and splitting on whitespace produces junk paths.
- **Never `--amend` in a shallow clone** — it folds changes into the single root
  commit. Reset onto `FETCH_HEAD` and commit cleanly instead.
- Cloned repos become **gitlinks**; register each in `.gitmodules` or a fresh clone
  gets empty directories.
- `libbz3.c` needs `-DVERSION="1.5.1"`; brotli must be compiled as **C**; brunsli's
  vendored copy is internally inconsistent and needs `false` added at two call sites.
- Verify a transform **engaged** (`flags`/`layers` byte), not just that output got
  smaller. This has caught four real bugs.

## 9. Standing rules

1. **Verify before use.** Never commit a transform whose round trip has not been
   checked on that exact input. This has turned five would-be corruptions into
   declined transforms.
2. **Measure, don't predict.** Three predictors have been wrong, all costing ratio:
   the sample selector (+2.68% size), the 256 KiB mid-sample, and this session's 2.1×
   pessimism. Where a structural fact about the input exists, use it.
3. **A transform pays only if it removes redundancy the backend cannot already
   capture.** This rule predicted every negative result.
4. **Licences:** ziptastic links → permissive only (LGPL via plugin, GPL never).
   heavenzip spawns → aggregation, each tool with its own licence. SREP has **no
   licence** and can never be shipped by us. Permission requests were sent to
   Ziganshin (SREP) and Walbeehm (DeflOpt) on 2026-11-01 — **check for replies**.
5. Document every phase, defect and measurement in `docs/`. Record failures as
   failures.
