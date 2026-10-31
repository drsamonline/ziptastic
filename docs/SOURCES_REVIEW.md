# Source review — what we have, and what each one is actually for

A survey of `vendor/hcs/old/`, written against our **measured** gaps rather than
against the phase plan in the abstract. Every number in the "why it matters"
column comes from `docs/BENCHMARKS.md` (Phase 5, `ziptastic --no-lzma2 -9`,
best-of-three entropy models, compared with `xz -9`).

The point of this document: we are +11.94% on the corpus, and that gap is not
uniform. It is concentrated in specific data types, and for several of them we
already have a source sitting in the tree that targets exactly that type. This is
the map from gap to source.

## Where we actually stand

| file | type | vs `xz -9` |
|---|---|---|
| `webster` | English dictionary | +3.47% |
| `dickens` | English text | +4.58% |
| `reymont` | Polish text / PDF | +5.78% |
| `nci` | chemical database | +6.90% |
| `xml` | XML | +7.50% |
| `osdb` | MySQL dump | +8.45% |
| `samba` | source tarball | +10.16% |
| `ooffice` | x86 DLL | +11.43% |
| `sao` | star catalogue (floats) | +13.15% |
| `mr` | MRI scan (16-bit ints) | +13.89% |
| `x-ray` | X-ray image (16-bit ints) | +16.22% |
| `mozilla` | tar of x86 executables | +19.31% |
| **total** | | **+11.94%** |

Two clusters dominate: **executables** (`mozilla`, `ooffice`) and **numeric
arrays** (`sao`, `mr`, `x-ray`). Together they are 5 of 12 files and they are
where the largest deltas are. Text is already close.

## What is in the tree, and whether it is usable

`vendor/hcs/SOURCES` is the manifest. "Integration" is its column, not my
opinion; where I disagree with it I say so.

| source | license | manifest says | present? | verdict for us |
|---|---|---|---|---|
| `xz-master` | 0BSD + LGPL-2.1 | link | yes, full | **in use** (liblzma = LZMA2 fallback). Also holds the BCJ filters — see below. |
| `lzma` (SDK) | Public-Domain | copy-src | yes | reference only; we already ported its BT4 finder. |
| `7z` | Public-Domain | copy-src | yes (`C/Ppmd7.c`) | **PPMd backend for text** (Phase 11). |
| `zpaq-master` | Public-Domain | link | yes (`libzpaq.cpp`) | **ratio ceiling** (Phase 10, `--max-ratio`). |
| `libbsc-master` | Apache-2.0 | link | yes | BWT backend (Phase 9). |
| `brotli-master` | MIT | link | yes | static-dictionary backend; strong on web text. |
| `zstd-dev` | BSD-3-Clause | link | yes | fast mode / fallback. |
| `lz4-dev` | BSD-2-Clause | link | yes | ultra-fast mode. |
| `srep-main` | Public-Domain | copy-src | yes (3.1k lines core) | **long-range matching** (Phase 6). Design reference; adapt, do not link. |
| `preflate-master` | Apache-2.0 | link | yes | **DEFLATE recompression** (Phase 8) — the archive gate. |
| `zopfli-master` | Apache-2.0 | link | yes | helps preflate re-encode DEFLATE streams. |
| `libdeflate-master` | MIT | link | yes | same role, much faster than zopfli. |
| `zlib-ng-develop` | zlib | link | yes | same role. |
| `bitshuffle-master` | MIT | **rewrite** | yes | **numeric pre-filter** — but the manifest says rewrite, so we write our own. |
| `pcodec` | Apache-2.0 | copy-src | **empty** (submodule not materialised) | numeric sequences; would need the submodule checked out. |
| `fse`, `ryg_rans` | BSD-3 / Public-Domain | copy-src | **empty** (submodules) | we wrote our own rANS instead; no longer needed. |
| `parmar-main` | **verify** | copy-src | yes | **do not use** — license is unverified, and the prompt forbids linking anything unverified. |
| `freearc-c-main`, `FA-master`, `freearc-main` | verify / mixed | reference | yes | reference only. |
| `packJPG-master`, `lzo-master`, `advancecomp-master` | LGPL/GPL | external-process | yes | **excluded by the prompt**. Not linkable. |
| `LPAQ9`, `FPAQF2`, `BARF`, `BBB`, `FLZP`, `SR3A` | unclear | — | single-file PAQ-family sources | interesting for the `--max-ratio` ceiling, but licenses need checking before any use. |

## Gap → source, in priority order

### 1. Executables: `mozilla` +19.31%, `ooffice` +11.43%

**Cause.** x86 code is full of relative CALL/JMP displacements. A 4-byte
displacement is different in every function, so the match finder and the literal
model both see noise where a human sees a jump table.

**Source we already have.** `xz-master/src/liblzma/simple/x86.c` — the BCJ
(Branch/Call/Jump) filter, 0BSD, already vendored *and* already linked, since we
link liblzma for the fallback. It rewrites relative displacements to absolute
ones so they repeat, and is exactly invertible.

**Why it was the best next move.** `xz -9` does **not** apply BCJ unless asked
(`xz --x86`), so this is a legitimate, measurable win against our own baseline.
It is a pure preprocessing transform — no format risk beyond a flag — and the
code already exists in the tree.

**DONE — and the prediction above was half wrong.** Implemented as
`src/precompressors/bcj_x86.cpp` (adapted from the 0BSD source, not linked) and
applied to the whole stream before anything else, so the filter state carries
across blocks exactly as it does inside `xz --x86`.

Measured: `ooffice` **2,704,203 → 2,328,361 (−13.90%)**, which puts it *below*
`xz -9` (2,426,816) — the first file in the corpus to beat the baseline.
`mozilla` got **0.78% worse**, not better. The reason is that `mozilla` is a tar
of executables: plenty of CALL/JMP bytes, but they are scattered across archive
members rather than forming one code image, so converting the displacements to
absolute makes them position-dependent without making them repeat.

So a density heuristic is not sufficient, and the code does not use one for the
decision. It tries the transform on a 128 KiB sample and keeps it only if the
sample actually codes smaller; `mozilla` is declined, `ooffice` is accepted, and
`dickens`/`samba` are never considered because the opcode density gate rejects
them first.

### 2. Numeric arrays: `x-ray` +16.22%, `mr` +13.89%, `sao` +13.15%

**Cause.** 16-bit samples and float arrays store one value across two or four
bytes. Consecutive samples are similar, but their *bytes* interleave high and low
halves, so byte-oriented matching sees high-entropy noise.

**Source.** `bitshuffle-master` — MIT, but the manifest says **rewrite**, so the
honest route is our own byte-transpose: split each element into its bytes and
concatenate the planes (all low bytes, then all high bytes). Trivially
invertible, needs no library at all, and is the single biggest lever left on
these three files.

**Action:** `src/precompressors/bitshuffle_wrap.cpp` as a *self-written* plane
splitter, applied when the block's byte-pair correlation suggests it, with the
element width recorded in the body.

### 3. Long-range redundancy: archives, `samba`, source tarballs

**Cause.** Our fine finder has a 64 MiB window and the parser only sees matches
inside it. A ZIP or tar that repeats a file 100 MB later is invisible.

**Source.** `srep-main` (public domain, 3.1k lines): 48-byte block hashing with
16-entry buckets over unbounded history. The prompt says adapt, not link, so the
design is the value — an index of block hashes beside the fine finder, queried by
the same parser, with a distance discount in the cost model.

**Action:** Phase 6. Gate from the prompt: `file_a` + 100 MB noise + `file_a`
again must be found, and beat fine-only by ≥30% on that input.

### 4. Deflate-heavy archives: the 3% gate

**Cause.** A ZIP/PNG/JAR is already DEFLATE-compressed; we are compressing
compressed data.

**Source.** `preflate-master` (Apache-2.0, linkable) detects DEFLATE streams,
inflates them, and stores a recipe that re-creates the exact original bytes —
so the inflated data (highly compressible) goes through our pipeline instead.

**Action:** Phase 8. This is the phase the prompt calls "the biggest ratio gate",
and the only realistic route to beating `xz -9` by 3% on `silesia.zip`.

### 5. Text: already close (+3.5% to +7.5%)

**Source.** PPMd from `7z` (public domain, `C/Ppmd7.c`) as an alternative
backend. LZMA-class coders are weak on natural language relative to PPM; this is
the standard second opinion, and the prompt's Phase 11 asks for it.

**Action:** Phase 11. Lower priority than 1–4 because the absolute gap is
smallest here.

### 6. Ratio ceiling

**Source.** `zpaq-master` (public domain, `libzpaq.cpp`) — context mixing, the
`--max-ratio` path (Phase 10). Slow, but it is how we approach the `paq8px`
class without touching GPL code.

## What we should *not* use, and why

- `packJPG`, `lzo`, `advancecomp`, `paq8px` (not vendored), `upx` — GPL/LGPL.
  The prompt forbids linking them and they are external-process-only in
  heavenzip. Nothing in `src/` may depend on them.
- `parmar` — license marked `verify` in the manifest. Not until someone checks
  it; the prompt is explicit that unverified sources do not get linked.
- `fse` and `ryg_rans` — empty submodules, and superseded: we wrote our own rANS
  and range coder, both tested. No reason to materialise them now.
- `pcodec` — empty submodule. Worth checking out if the numeric work in item 2
  is not enough, but bitshuffle-style transposition should be tried first because
  it needs no dependency at all.

## Recommended order

1. **BCJ x86** — smallest change, attacks our two worst files, source already
   vendored and linked. Do it first.
2. **Byte-transpose for numeric data** — three files, no dependency, ~150 lines.
3. **Phase 6 coarse index** — the plan's next phase; unlocks archives.
4. **Phase 8 preflate** — the gate that decides whether the project's headline
   claim (beat `xz -9` by 3% on Deflate-heavy archives) is reachable.

Everything above is permissive-licensed or self-written. Nothing in this list
requires touching a GPL/LGPL source, and nothing requires a network call at
runtime.
