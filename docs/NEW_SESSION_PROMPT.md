# Paste this to start a new session

---

You are continuing work on **ziptastic**, a lossless compressor by Dr Sohil Momin
(GitHub: drsamonline). Read this whole message before doing anything.

## Read first, in this order

1. `docs/SESSION_HANDOFF.md` — state, measurements, next steps, gotchas. **This is
   the authority; if anything below disagrees with it, it wins.**
2. `docs/BENCHMARKS.md` — every measured result.
3. `docs/AI_REVIEWS_SYNTHESIS.md` — twelve AI reviews, what was accepted and rejected.
4. `docs/ALGORITHMS_FROM_PAPERS.md` — ranked techniques, plus the "measured and
   closed" table.

## What the project is

ziptastic is a lossless archiver whose edge is **not** its entropy coder but its
**Layer 0**: before compressing, it parses the *container* the data is in, transforms
it into something more compressible, and stores a recipe that reproduces the original
bytes exactly.

The governing finding, measured repeatedly: **container coverage pays 19–79%;
entropy-coder work pays 1–3%.** Every large win came from parsing a format.

Measured (through the shipped CLI, all round-trip exact):
- Recursion on a nested `.tar.gz` → `.xlsx`: **−50.7%**
- PDF embedded DEFLATE −79.1%; integer CSV/TSV −77.4%; GIF −29.5%; JPEG −19.4%;
  ZIP/DEFLATE −28.8% vs `xz -9`
- LZ4 28.8–58.2% and base64 −4.5%/−71.8% measured but **not yet integrated**
- Silesia corpus: −11.83% vs `xz -9`

Sibling repos: **heavenzip** (orchestrator that spawns binaries; docs only) and
**lexora** (text canonicalisation + compression). Cross-repo notes live in
`docs/LEXORA_ASSESSMENT.md` here and `lexora/docs/ZIPTASTIC_NOTES.md` there.

## Set up the environment first

The sandbox recycles between sessions and turns. Everything below is needed before
you can build:

```bash
cd /scratch && mkdir -p repos && cd repos
git clone --depth 1 https://github.com/drsamonline/ziptastic.git
cd /scratch/work && pip install --quiet cmake ninja ziglang
mkdir -p toolchain && cd toolchain
ZIG=/scratch/.local/lib/python3.12/site-packages/ziglang/zig
for n in cc ar; do printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s %s "$@"\n' "$ZIG" "$n" > zig-$n; chmod +x zig-$n; done
printf '#!/bin/sh\nexport ZIG_GLOBAL_CACHE_DIR=/scratch/work/.zig/g\nexport ZIG_LOCAL_CACHE_DIR=/scratch/work/.zig/l\nexport HOME=/scratch/work\nexec %s c++ "$@"\n' "$ZIG" > zig-cxx; chmod +x zig-cxx
```

Configure and build **in separate calls** — each exceeds the 300 s command cap:

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

## Your task

**Implement the XML-aware numeric transform.**

Recursive container descent landed, but the nested numeric transform correctly does
not fire on an `.xlsx`, because the inflated payload is XML:

```xml
<row r="1"><c r="A1"><v>1003</v></c><c r="B1"><v>500123</v></c></row>
```

— not delimited text, so the CSV transform has nothing to bite on. That is the
difference between the current **−50.7%** and the **−89.4%** measured on this file
class. Highest value, best defined.

Requirements:
- Recognise `<v>` numeric cell values in spreadsheet XML, in column order.
- Reuse the delta + zigzag + varint encoding proven in
  `src/precompressors/numeric_wrap.cpp`.
- **Exactness is the whole contract**: parse → re-serialise → byte-compare, decline
  the column on mismatch. Never normalise.
- New layer bit alongside `kLayerNumeric` / `kLayerNumericNested`.
- Verify before use, at every level.

When that is done, the queue continues at `docs/SESSION_HANDOFF.md` §6 — per-block
integrity, validate-before-allocate, base64, LZ4, gzip.

## Rules that have earned themselves

1. **Verify before use.** Never commit a transform whose round trip has not been
   checked on that exact input. Five would-be corruptions turned into declines.
2. **Measure, don't predict.** Three predictors wrong, every time costing ratio: the
   sample selector (+2.68% size), the 256 KiB mid-sample, and a 2.1× pessimism that
   blocked a −50.7% transform. Where a structural fact about the input exists, use it.
3. **A transform pays only if it removes redundancy the backend cannot already
   capture.** This predicted every negative result in the project.
4. **Check the transform actually engaged** — read the `flags`/`layers` byte — not
   just that output shrank. Four real bugs caught this way.
5. **Document everything**: every phase, defect and measurement into `docs/`,
   failures recorded as failures.

## Working constraints

- **`git push` is 403.** Commit via `GITHUB_COMMIT_MULTIPLE_FILES` in `run_program`,
  using `sarvam_tools`.
- The commit path is **text-only UTF-8** and silently skips non-UTF-8 files
  (transcode first); binaries cannot be committed — embed fixtures as byte arrays.
- Use `git diff-tree --name-only -z` and split on NUL.
- **Never `--amend` in a shallow clone** — reset onto `FETCH_HEAD` and commit cleanly.
- Register cloned repos in `.gitmodules` or a fresh clone gets empty directories.
- Verify tree hashes after every push.

## Licence policy

ziptastic **links**, so permissive-only: MIT/Apache/BSD directly, LGPL through the
plugin ABI (`include/chimera/plugin.hpp`), **GPL never**. SREP has no licence at all
and can never be shipped. Permission requests to Ziganshin (SREP) and Walbeehm
(DeflOpt) went out 2026-11-01 — **check for replies**.

## Start by

Confirming the build and tests pass, reading `docs/SESSION_HANDOFF.md` and
`docs/RECURSION_MEASUREMENT.md`, then proposing your implementation plan for the
XML-aware numeric transform before writing code.
