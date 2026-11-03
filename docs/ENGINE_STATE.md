# State of the engine — everything in one place

Written 2026-11-02. This is the map: every Layer 0 transform, its bit, what it measured,
and where the write-up lives. `SESSION_HANDOFF.md` is the working document (what is next
and why); this one is the reference.

## Format

**v5.** `"CHCE" | version | flags | layers | layers2 | original_size | block_count |
crc32`, per block `mode | usize | csize | sha256[32] | metadata | payload`, then a `ZTIX`
trailer carrying the block count and a digest of the block manifest. Both `layers` bytes
are full (see below). Streams older than v5 are refused with `bad-version`.

`original_size` is the size **after** Layer 0, not the caller's byte count. The CRC-32 is
over the true original and is what guards the round trip.

## Layer 0 transforms — `layers` (full)

| bit | transform | measured | doc |
|---|---|---|---|
| 0x01 | integer CSV/TSV columns, delta+varint | −77.4% | `NUMERIC_RESIDUAL_EXPERIMENT.md` |
| 0x02 | the same, applied inside a container | — | `RECURSION_MEASUREMENT.md` |
| 0x04 | spreadsheet XML cell values | **−50.3%** on an `.xlsx` | `XML_NUMERIC_MEASUREMENT.md` |
| 0x08 | the same, inside a container | −51.2% on the sheet XML | same |
| 0x10 | base64 runs decoded | −19.5% (JPEG) / −48.1% (wrapped XML) | `BASE64_INTEGRATION.md` |
| 0x20 | gzip members inflated | −21.2% / −25.4% | `GZIP_INTEGRATION.md` |
| 0x40 | second container level (tar→ZIP) | **−44.8%** on `.tar.gz`→`.xlsx` | `NESTED_CONTAINERS.md` |
| 0x80 | WAV PCM per-channel delta | −16.4% (16-bit music), −14.2% (24-bit), −6.1% (float) | `WAV_MEASUREMENT.md` |

## Layer 0 transforms — `layers2` (bits 0–4 used, 5–7 free)

| bit | transform | measured | doc |
|---|---|---|---|
| 0x01 | SQLite pages grouped by type | −4.8% / −6.3% | `SQLITE_MEASUREMENT.md` |
| 0x02 | base64 one level down | −19.7% with 0x04 | `BASE64_NESTED.md` |
| 0x04 | JPEG one level down (brunsli) | −19.7% on `.json.gz` | same |
| 0x08 | GIF one level down | — | same |
| 0x10 | PNG image data inflated | **−22.9%** sprite / **−68.8%** gradient | `PNG_MEASUREMENT.md` |

## Container work that predates the bit map

- **precomp** (preflate): ZIP members and PDF streams, `kFlagPrecomp`.
- **recursion**: precomp re-entered on what a container inflated, `kLayerPrecompNested`.
- **JPEG** (brunsli) and **GIF** at the top level, `kFlagJpeg` / `kFlagGif`.
- **dedup** (SREP-style) and **BCJ x86**, `kFlagDedup` / `kFlagBcj`.
- **SFX**: a stream behind a launcher stub decodes, because `decompress()` finds the
  `CHCE` magic rather than requiring offset 0.

## Backends and selection

Per block, the smallest of: RAW, rANS (parsed), static Huffman, adaptive Huffman
(**off by default**, `HUFFMAN_GATE.md`), range, BWT (libbsc), PPMd, LZMA2 fallback
(Tier-1 guarantee), and CM/zpaq under `--max-ratio`. Selection is per 4 MiB block, so
"sometimes 7z, sometimes arc" is answered at a finer grain than a file.

## CLI

`compress` (default), `decompress`, `bench`, `version`, `help`. Flags worth knowing:
`-9`, `--max-ratio`, `--parallel`, `--split-size N`, and `--no-<transform>` for every
Layer 0 stage (`--no-precomp`, `--no-numeric`, `--no-base64`, `--no-wav`, `--no-sqlite`,
`--no-bwt`, `--no-huffman`, `--huff-adapt`, …). `--split-size` writes `.001`… with a
16-byte footer and the decoder joins the siblings.

## Tests

29 suites. Unit coverage per transform, plus `integration.cli` (35 assertions) driving the
real binary: round trips, layer-bit assertions, split parts, corruption paths.

## Measured negatives — do not rebuild these

| idea | result | doc |
|---|---|---|
| LZ4 precompression by storing the parse | **+50%** on real LZ4 data | `LZ4_MEASURED.md` |
| "zip store" unpacking | **−1.4%** (it is worse) | `ZIP_STORE_MEASURED.md` |
| split archives, per part | **~2x worse**; belongs to heavenzip | `SPLIT_ARCHIVES.md` |
| parallelising the selector | wrong lever; two candidates are waste | `SELECTOR_PROFILE.md` |
| Tier-1 fallback on a second thread | **9% slower** (2 cores, 2 GiB) | `CONCURRENT_FALLBACK.md` |
| adaptive Huffman | 22% of the run, never a byte smaller | `HUFFMAN_GATE.md` |
| base64 of single-line structured text | **−6.3%** (a loss; the gate declines it) | `BASE64_INTEGRATION.md` |

## Open, measured, not built

- **ECM for raw CD images — 30.8%** (`ECM_MEASUREMENT.md`). The largest unclaimed win.
  Needs RS(26,24) over GF(2⁸), and the honest blocker is that conformance cannot be
  verified here: there is no real disc image in the sandbox and a self-generated one
  would only prove the implementation agrees with itself.
- **SFX writing** — needs a launcher per platform; antivirus and signing are real
  (`SFX_AND_SPLIT_OUTPUT.md`).
- **CM backend** last: 1–3% ceiling, hundreds of times slower.
- **WAV chunk-level** work (LIST/INFO, `fact`), and audio formats beyond PCM/float.

## Against the field

| file | `-9` | zstd `-19` | difference |
|---|---|---|---|
| 2 MB English prose | 408,418 | 546,717 | **−25.3%** |
| 6.8 MB sheet XML | 70,086 | 390,673 | **−82.1%** |
| 4.8 MB cooked ISO | 3,333,900 | 3,352,732 | **−0.6%** |

`docs/VS_THE_FIELD.md`. The pattern matters more than the headline: the gain comes from
format-aware transforms, and on data with neither a format nor redundancy there is almost
nothing to win. **The LZMA-class comparison is now measured** against `xz -9e` via liblzma
(Python's `lzma`, preset 9 + EXTREME): **+24.9%** on prose, **+57.2%** on sheet XML, **+29.6%**
on a raw CD image, **+0.2%** on a cooked ISO, and **−0.0%** on a floppy image and an
asset-heavy near-duplicate tree. Where a transform fires the win is structural; where none
fires it is a tie, and twice marginally behind.

## Tools suggested from outside, and what they would add

`docs/COMPETING_TOOLS.md`. **lrzip/rzip** would not replace the srep step: `dedup` already
lands within **0.1% of the ideal** on both a 3 MB duplicate at 4 MB distance and 200
duplicates spread over 9 MB. **Near-duplicates were then tested and are not a gap either** —
`dedup` does not fire on them, but the backend's match finder already handles them: we beat
zstd by 1.6% on an asset-heavy near-duplicate tree and trail it by 1.1% on a mixed one. **oxipng/optipng** optimise the file, not the archive — a 66%
smaller PNG changed the archive by 3 bytes, because the PNG transform stores pixels, not
deflate choices. **browser-image-compression** is lossy and therefore a different product
decision.

## The three habits this project keeps re-learning

1. **Measure before building.** Every transform here that shipped had a number before it
   had code, and four items died on the measurement instead.
2. **Verify before use.** Every transform rebuilds its input and byte-compares. That is
   what caught the SQLite permutation bug, the PNG zlib wrapper, and the `-0` hole in the
   shared integer parser.
3. **A prototype that only reports a size can be wrong.** The 24-bit WAV prototype
   reported −56.7% because it dropped half the samples; the real number is −14.2%.
