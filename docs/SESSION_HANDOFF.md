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
| ziptastic | `4794908` + WAV | builds clean, **27/27 suites pass**, format **v4** |
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
| base64 → bytes, MIME-wrapped | −20.0% (JPEG) / −48.1% (wrapped XML) |
| base64 → bytes, single-line structured text | **declined**, it loses 6.3% |
| LZ4 payload extraction | −28.8% to −58.2% ideal; **+49.7% worse** once the parse is stored — see `LZ4_MEASURED.md` |
| GIF LZW → indices | −29.5% |
| ZIP/DEFLATE (preflate) | −28.8% vs `xz -9` |
| gzip members inflated | −21.2% (XML) / −25.4% (tar.gz of text) |
| WAV 16-bit PCM, per-channel delta | −16.4% (music) / −2.0% (noise SFX) |
| **tar.gz → tar → xlsx → XML** | **−44.8%** (107,221 → 59,183), within 233 B of the inner file alone |
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
5. **Validate-before-allocate — shipped.** `block_count` is now bounded by the
   bytes actually in hand (`remaining / kMinBlockHeaderBytes`) before anything is
   allocated for it, plus a `kMaxBlocks` ceiling in `read_file_header`. An
   83-byte stream declaring 10M blocks was asking for ~914 MB; it is now refused
   in 13 ms. `docs/VALIDATION.md`.
6. **base64 transform — shipped.** Wrapped base64 is worth −20.0% on a dense
   payload and −48.1% on structured text; single-line structured text is a 6.3%
   loss and the trial declines it. The gate took three attempts, all of them
   measuring something other than what the engine does — read
   `docs/BASE64_INTEGRATION.md` before touching it. Costs ~2.7× compress time on
   the files where it runs.
7. **gzip — shipped.** A gzip member is a 10-byte header, one bare DEFLATE stream and
   an 8-byte trailer, so this reuses preflate entirely; the new work is the container
   and multi-member handling. `.tar.gz` was the biggest hole in Layer 0 — a tar.gz used
   to compress to *larger than the file*. Now: `sheet.xml.gz` −21.2%, `logs.tar.gz`
   −25.4%, `book.tar.gz` −30.0%. `docs/GZIP_INTEGRATION.md`.
8. **WAV per-channel delta — shipped.** −16.4% on tonal music, −2.0% on noise;
   mid/side measured worse on all three fixtures so the simpler form ships.
   `docs/WAV_MEASUREMENT.md`. **`kLayerWav` is bit 7 — every layer bit is now
   allocated.** The next Layer 0 capability needs a second `layers` byte and a format
   version bump: plan it as its own item.
9. **XML-aware numeric transform — shipped.** Spreadsheet cell values are now coded
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
3. ~~**Validate-before-allocate in `read_file_header`**~~ **DONE** — `docs/VALIDATION.md`.
   One instance is left and it is documented rather than fixed: `dedup_unpack` caps
   its declared output at 1 TiB, so a crafted payload can still ask for a huge
   reserve. It sits behind the per-block digests now, but give it the same
   structural treatment when the Layer 0 chain is next touched.
4. ~~**Base64 transform**~~ **DONE** — `docs/BASE64_INTEGRATION.md`. Bit 5 is free
   for the **nested** variant (base64 inside a ZIP), which is the obvious next step
   and is where a JSON API response in a container would land. The gate is also the
   expensive part: ~2.7× compress time on the files where it runs, and a cheaper
   gate that is still correct would be worth real money.
5. ~~**LZ4 precompression**~~ **MEASURED, NOT SHIPPED** — `docs/LZ4_MEASURED.md`.
   The −28.8%/−58.2% figures were the ideal case with a free parse. Storing the parse
   costs about what LZ4's own descriptors cost, so the transform goes **+49.7% worse**
   on spreadsheet XML and **+50.9% worse** on 4 MB of logs; it wins only where LZ4
   found few long matches (52 sequences for 1.65 MB of JSON, −27.7%). The recipe is
   not the fix: dropping the derivable match lengths leaves 72–76% of it.
   **The salvageable route is re-encode-and-compare** — store only the parameters and
   accept only when a bit-exact LZ4 encoder reproduces the stream, which needs
   liblz4's greedy matcher reproduced exactly. Try that before writing this item off.
6. ~~**gzip / multi-member gzip**~~ **DONE** — `docs/GZIP_INTEGRATION.md`. It unblocks
   the nested chain but does not finish it: gzip hands back a **tar**, and the tar holds
   the xlsx. `book.tar.gz` gains 30%, not 89%, because reaching the sheet XML needs a
   tar parser and a second precomp pass. That is item 6a.
6a. ~~**tar parsing inside the inflated payload.**~~ **DONE** — `docs/NESTED_CONTAINERS.md`.
   No tar parser was needed: `scan_zip` now finds local file headers **anywhere** in the
   buffer instead of requiring offset 0, and the recursion re-enters `preflate_unpack` on
   what gzip inflated. `book.tar.gz` 107,221 → **59,183** (−44.8%), within 233 bytes of
   compressing the inner `.xlsx` directly. The same change makes **SFX** archives (6b)
   visible for free. **`layers` is now full**: the next Layer 0 capability needs a second
   `layers` byte, which is a format change — plan for it.
6a-bis. ~~**Stored-ZIP unpacking — the "zip store" step.**~~ **MEASURED, REJECTED** —
   `docs/ZIP_STORE_MEASURED.md`. A `zip -0` container of a game-like tree compresses to
   374,087 as-is, 379,460 with the container stripped off and 404,076 with the members
   compressed individually: **unpacking costs 1.4%**, because the zip's headers and
   central directory are redundant and the engine already collects that, and because
   dedup across near-duplicate members only works when the container is one stream.
   The nested-container case — the only real argument for it — was isolated at 526
   bytes (2.7%) on a container that is 99% one nested file. Dropped.
   The step does not carry over because the repack tools are file-oriented and
   ziptastic is not: `zip store` exists to stop the container hiding bytes from
   precomp/srep/7z, and nothing is hidden here. **precomp on a deflated zip was
   confirmed working in the same run** (−6.2%, `flags` 0x25), contrary to the field
   impression that it often does nothing.
6b. ~~**SFX (self-extracting archives).**~~ **DONE, as a side effect of 6a.** The
   anywhere-scan finds the ZIP behind an `.exe` stub, and the stub rides along as part of
   the stored skeleton. Covered by `a_zip_after_a_stub_is_found`. What is *not* done is
   finding the ZIP from its end-of-central-directory record when the local headers are
   damaged or the stub contains a decoy `PK\x03\x04`; that refinement is not urgent.
6c. **Split archives.** **Note before starting:** ziptastic takes one file, and a
   split archive is several. `.001/.002` byte-splits are just the original file cut up,
   so a concatenation already works today — the anywhere-scan in `scan_zip` finds the
   ZIP at offset 0. A multi-volume `.z01/.z02/.../.zip` is different: its central
   directory is in the last part and local headers are spread across the others, so
   concatenation does not reconstruct a plain ZIP. The honest split is: ziptastic
   declines a lone fragment cleanly (it already does), heavenzip drives the whole set.
   Measure first, as with LZ4 — it may be an orchestrator item rather than an engine
   one. `.z01/.z02/…/.zip` multi-volume ZIPs, and `.001/.002` byte-splits.
   Two different jobs: a byte-split is trivially reassembled and re-split from stored
   part sizes; a multi-volume ZIP has its central directory in the last part and local
   headers spread across the others, so the container parse has to span parts and the
   recipe has to record each part's length. Worth doing after SFX, which shares the
   "find the ZIP without assuming offset 0" work.
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
- **heavenzip's job, sharpened by field practice** (`docs/REPACK_PIPELINES.md`): the
  hand pipeline `zip store -> precomp -> srep -> 7z|arc` wins, but which of 7z and arc
  wins changes per job, and the imported repack scripts differ per game. That is a
  **pipeline search** problem, not a codec problem: enumerate
  {store, deflate} × {precomp on/off} × {dedup params} × {7z, arc, zpaq}, measure, and
  keep per-title profiles. heavenzip can do it because it spawns; ziptastic cannot
  because SREP and ARC are not linkable.
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
