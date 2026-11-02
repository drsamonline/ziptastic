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
| ziptastic | `cd9f2a5` + SQLite | builds clean, **28/28 suites pass**, format **v5** |
| heavenzip | `88ac3d6` | docs only, no implementation |
| lexora | `ad57991` | Phase-1 prototype, 502 tests local |

ziptastic `docs/` holds 20 files. Start with `BENCHMARKS.md`,
`AI_REVIEWS_SYNTHESIS.md`, `RECURSION_MEASUREMENT.md`, `LEXORA_ASSESSMENT.md`.

**Real-world test images** are described in `docs/TEST_CORPUS.md` — what each one actually
is, what measured, and the important finding that the supplied `.raw` is a **disk** image
(2,048-byte sectors, boot sector at 0), not a 2,352-byte CD image, so **ECM still has no
test material**.

**Everything in one place:** `docs/ENGINE_STATE.md` — every transform, its layer bit, its
measured gain, its write-up, the measured negatives, and what is open. This handoff is the
working document; that one is the reference.

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
| WAV 24-bit and 32-bit float | −14.2% / −6.1% on the same music signal |
| SQLite pages grouped by type | −4.8% (295 KB) / −6.3% (3.46 MB) |
| PNG image data inflated (preflate) | **−22.9%** (sprite) / **−68.8%** (gradient) |
| **CD sector parity regenerated (ECM)** | **−28.2%** on a real Mode 2 Form 1 image |
| **BMP colour planes + row delta** | **−98.9%** photographic / **declined** on a sprite |
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

Format **v5**: magic, version, `flags` (8 bits, **full**), `layers`, `layers2`
(Layer 0 bits — the first byte is full), original_size, block_count, CRC-32; per
block a 32-byte SHA-256; then a `ZTIX` trailer carrying the block count and a digest
of the block manifest. `layers2` has no bits allocated yet: it exists so the next
Layer 0 transform is not blocked on a format change. v4 and earlier streams are no
longer readable — the same deliberate break as v1→v2→v3→v4.

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
9. **Format v5 — a second `layers` byte.** The first is full (eight bits, all
   allocated by WAV), so the ninth Layer 0 transform had nowhere to live. `layers2`
   is on disk, validated against `kLayerMask2` (currently 0), and unused. This is
   plumbing, not ratio — it unblocks base64-nested, SQLite pages and whatever comes
   next. Two tests hardcoded header offsets and needed updating; that is the cost of
   a format field and it is worth knowing before the next one.
10. **SHA-NI for the per-block digest.** The digest cost 28.4 ms per 4 MiB block on the
   scalar path (140.8 MB/s) and was quoted at ~22% of decode. A SHA-NI kernel selected
   by CPUID now runs at **708.5 MB/s (5.0x)**, 5.65 ms per block; the same 3.25 MB
   decode is **102.7 → 97.8 ms (−4.8%)** and the digest is **~5% of wall time**. The
   22% was an extrapolation from throughput, not a measured delta — the end-to-end
   saving is smaller because the digest overlaps with I/O. Both numbers are in
   `INTEGRITY.md`. The scalar path stays as the fallback and is pinned by a test
   (`sha256_scalar`), because a fallback nothing runs is a fallback that rots.
11. **SQLite page grouping — shipped.** Pages emitted grouped by B-tree type with a
   2-byte-per-page permutation: −4.8% and −6.3% on two databases. First user of the
   v5 `layers2` byte. `docs/SQLITE_MEASUREMENT.md`. Two traps recorded there: the
   usual "packed must be smaller" rule silently disabled it (a reordering is larger on
   disk by construction), and the test fixture overwrote its own SQLite magic.
12. **base64 one level down — shipped.** `kLayer2Base64Nested` (layers2 bit 1). Fires on
   a `.json.gz`: gzip inflates to a document whose payload is a base64 blob. Measured
   **−0.7%** on a base64-of-a-JPEG fixture against **−19.5%** for the same document
   uncompressed — the gap is that the nested chain re-enters precomp, base64 and the
   numeric transforms but **not the image transforms**, so brunsli never sees the blob.
   `docs/BASE64_NESTED.md`.
12b. **Nested JPEG and GIF — shipped the same day.** The 0.7% was the nested chain not
   running the image transforms: a decoded JPEG was handed to the backend as ordinary
   bytes. With `kLayer2JpegNested` / `kLayer2GifNested` the same `.json.gz` goes
   **21,688 → 17,414 (−19.7%)**, marginally better than the document uncompressed.
   `layers2` 0x06 = base64-nested + jpeg-nested.
12c. **PNG — shipped.** `kLayer2Png` (layers2 bit 4). A PNG is a zlib stream in a chunk
   list, so this is the gzip pattern applied to a format precomp never reached:
   **−22.9%** on a 2 KB sprite and **−68.8%** on a 377 KB gradient. `docs/PNG_MEASUREMENT.md`.
   Two traps, both caught by the transform declining 100% of inputs at first: the IDAT
   stream is zlib-**wrapped** (preflate parses bare deflate, so the 6-byte wrapper is
   peeled and stored), and an IDAT chunk's CRC is not in the skeleton, so it travels with
   the chunk length.
13. **XML-aware numeric transform — shipped.** Spreadsheet cell values are now coded
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
6c. ~~**Split archives.**~~ **MEASURED, NOT AN ENGINE ITEM** — `docs/SPLIT_ARCHIVES.md`.
   Compressing parts separately costs about 2x: a 1.8 MB tar is 10,263 whole and 20,857
   as two halves; a 225 KB zip is 10,445 whole and 30,425 as two halves. Concatenating
   the parts reproduces the original exactly and the whole-file gain comes straight
   back. The loss is redundancy *between* parts, which no single-file compressor can
   see — a `.001/.002` split cuts through DEFLATE streams so precomp can inflate
   neither half, and a multi-volume ZIP keeps its central directory in the last part.
   ziptastic already handles a lone fragment correctly (exact, just ~2x worse). The
   work belongs in **heavenzip**: detect the set, join it, compress, then re-split from
   stored part sizes.
6d. **Split the OUTPUT, and SFX.** Both answered in `docs/SFX_AND_SPLIT_OUTPUT.md`.
   SFX **reading** is shipped: `decompress()` finds the `CHCE` magic rather than
   requiring offset 0, so a stub + stream decodes with no special case. SFX **writing**
   is not built and should not be built blindly — a launcher per platform, AV
   heuristics and code signing are all real, and the format does not need us to ship
   binaries. Split **output is now built**: `--split-size N` writes `.001`… with a
   16-byte footer (index, count, total, `"ZS01"`) and `ziptastic d <any part>` joins the
   siblings; a missing part is named rather than surfacing as a truncated stream.
7. ~~**Parallelise the try-everything selector**~~ **MEASURED — the wrong lever.**
   `docs/SELECTOR_PROFILE.md`. On a 1 MB text file the candidates that cost time are
   **Huffman (18%)** and the **LZMA2 fallback (34%)**, and both produce byte-identical
   output; BWT, which looked guilty in a bad first profile, costs 0.6% and *wins* 2.6%.
   On two cores a fan-out buys at most 2x on a partly serial stage, so the real fix is
   to **gate** Huffman with a sample trial and to sample the LZMA2 comparison before
   paying for a full run — neither built, both need the trial to be right.
   `--no-bwt` and `--no-huffman` now exist as CLI flags so this can be re-measured.
7a-bis. ~~**Run the Tier-1 fallback concurrently with the native encode.**~~ **MEASURED,
   REVERTED** — `docs/CONCURRENT_FALLBACK.md`. The two encodes are independent and
   sequential costs 3,368 ms (1,657 native + 1,711 fallback) on 1 MB of prose, so a clean
   overlap should give ~1,750 ms. It gave **3,681 ms — 9% worse**. Nothing was shared
   between the threads; this box has 2 cores and 2 GiB, and the two working sets do not
   fit together, so the memory system serialises what the scheduler overlapped. Retry
   when the machine changes.
7b. **Adaptive Huffman gated off — done the same day.** `docs/HUFFMAN_GATE.md`. Splitting
   the candidate in two showed the cost is entirely the adaptive half: 858 ms of 3,867 ms
   (22%) against 98 ms for static. On four shapes — prose, 90% zeros, a seven-symbol
   alphabet, low-entropy binary — it never produced a smaller stream; the one Huffman win
   (0.91% on the seven-symbol file) came from the **static** candidate. `huff_adaptive`
   is now off by default: same output everywhere, **28-41% faster** on three of the four.
   `--huff-adapt` turns it back on.
8. WAV/PCM per-channel delta, then SQLite pages.
9. **ECM for raw CD images — MEASURED AT 30.8% synthetic, to be re-measured**
   (`docs/ECM_MEASUREMENT.md`). **The verification blocker is gone** —
   `docs/ECM_TEST_MATERIAL.md`: a real Mode 2 Form 1 image (2,000 sectors × 2,352, genuine
   EDC/ECC) was generated with `mkpsxiso`'s EDC/ECC class (GPL-2.0, used only to produce
   data, nothing entering the repo). The transform can now be checked by recomputing the
   276 ECC bytes and comparing them with the image's own — the check that was missing, and
   the one without which a wrong implementation would still round-trip perfectly by
   declining every real image. **The transform is written and verifies** —
   `docs/ECM_IMPLEMENTATION.md`: all 2,000 sectors of the real image reproduce their
   parity exactly, 4,704,000 → 4,154,002, round trip byte-exact. **Not yet wired in**:
   next commit takes `kLayer2Ecc` (layers2 bit 5), the stage, the inverse and the CMake
   entry, then measures on the real image. **DONE — and it is −28.2%** on the real
   2,000-sector image (1,973,243 → 1,417,448, round-trip exact, `layers2` 0x20), within
   1.2% of compressing the user data alone. The synthetic estimate of 30.8% was 2.6 points
   optimistic: real parity is a deterministic function of the data and carries a little
   structure the backend can use, where the synthetic bytes were uniform random.
   The 12% was uncompressed arithmetic (608 KB of 4.7 MB); RS parity is incompressible,
   so in *compressed* terms it costs 592,620 of 1,924,173 bytes. Measured on a synthetic
   raw Mode-1 image with a realistic user-data mix: 1,924,173 with ECC vs 1,331,553 for
   the user data alone. **This is the largest unclaimed win in the queue** and should be
   the next Layer 0 item. Building it means RS(26,24) over GF(2^8) — P over 43 columns,
   Q over 26 diagonals, 288 bytes/sector — regenerated on decode and byte-verified, plus
   the Mode-2-form-1 layout. Cooked `.iso` has no ECC and is unaffected.
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
