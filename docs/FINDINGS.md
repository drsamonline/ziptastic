# Findings: the single authoritative record

Written 2026-11-04. **This document supersedes the per-item write-ups in this directory.** Those
are the history — how each number was arrived at, what failed on the way, what was corrected —
and several of them contain figures that were later revised. Where this document and an older
one disagree, **this one is right**.

The reason it exists: after a long session the directory held 57 documents, some of them with
three successive corrections appended, and a reader could find −99% and −37% for the same
transform in two places without any indication of which was current.

---

## 1. What the engine is

Format **v7**. Header: `"CHCE" | version | flags | layers | layers2 | layers3 | original_size |
true_size | block_count | crc32`, per block `mode | usize | csize | sha256[32] | metadata |
payload`, then a `ZTIX` trailer with the block count and a manifest digest.

- **`original_size`** is the size *after* Layer 0; **`true_size`** is what the caller gets back.
- Three layer bytes, because each was filled as it was needed. `layers3` has seven free bits.
- Nine backends per block (RAW, rANS, static Huffman, range, BWT, PPMd, LZMA2 fallback, CM under
  `--max-ratio`), chosen by smallest output. Adaptive Huffman is off by default.
- 32 test suites, all passing. A full audit of `compressor.cpp` and `decompressor.cpp` is
  recorded in section 6.

## 2. Layer 0 transforms, with the numbers that are current

Every figure below is post-compression: how much smaller the *archive* got.

| transform | bit | measured | fixture |
|---|---|---|---|
| integer CSV/TSV columns | layers 0x01 | **−77.4%** | a whole CSV file |
| spreadsheet cell values | layers 0x04 | **−50.3%** | `.xlsx`, 2×40,000 ints |
| the same, inside a container | layers 0x08 | −51.2% | the inflated sheet XML |
| base64 runs decoded | layers 0x10 | **−19.5%** | base64 of a 21 KB JPEG |
| gzip members inflated | layers 0x20 | −21.2% / −25.4% | `.xml.gz`, `.tar.gz` |
| second container level | layers 0x40 | **−44.8%** | `.tar.gz` holding an `.xlsx` |
| WAV PCM per-channel delta | layers 0x80 | −16.4% / −14.2% / −6.1% | 16-bit / 24-bit / float |
| SQLite pages by B-tree type | layers2 0x01 | −4.8% / −6.3% | 295 KB / 3.46 MB db |
| base64 one level down | layers2 0x02 | −19.7% | `.json.gz` with a JPEG |
| JPEG one level down | layers2 0x04 | see section 4 | |
| GIF one level down | layers2 0x08 | see section 4 | |
| PNG image data inflated | layers2 0x10 | −22.9% / −68.8% ⚠️ | synthetic PNGs |
| **CD sector parity (ECM)** | layers2 0x20 | **−28.2%** | real Mode 2 Form 1 image |
| BMP colour planes + row delta | layers2 0x40 | **−36.9% / −48.3%** | photographic BMPs |
| PNG one level down | layers2 0x80 | −79.2% ⚠️ | ZIP of synthetic PNGs |
| **BMP one level down** | layers3 0x01 | **−36.9% / −48.3%** | ZIP of photographic BMPs |

⚠️ **Synthetic fixture.** The PNG figures (and the GIF/JPEG nested ones in section 4) were
measured on generated gradients whose pixels are a formula. Those compress to almost nothing
under any transform, so the numbers overstate what real content will do. The BMP row was
corrected for exactly this reason: its first figure was −99.2%, and re-measurement on
photographic content gave −37% to −48%. **PNG and GIF have not yet been re-measured and should
be assumed to be in the same 30–50% band until they are.**

## 3. Against the field

| file | ziptastic `-9` | zstd `-19` | `xz -9e` |
|---|---|---|---|
| 2 MB English prose | 408,421 | 546,717 (+25.3%) | 543,572 (+24.9%) |
| 6.8 MB sheet XML | 70,090 | 390,673 (+82.1%) | 163,656 (+57.2%) |
| 4.7 MB raw CD image | 1,417,452 | — | 2,014,224 (+29.6%) |
| 4.8 MB cooked ISO | 3,333,904 | 3,352,732 (+0.6%) | 3,339,112 (+0.2%) |
| 1.4 MB floppy image | 1,257,830 | — | 1,257,792 (−0.0%) |
| 8 MB near-duplicate asset tree | 228,012 | — | 227,972 (−0.0%) |

**Where a format transform fires, the win is large and structural. Where none fires, it is a
tie, and twice marginally behind.** On data with neither a format to parse nor redundancy a
general compressor misses, ziptastic is an LZMA-class compressor with more machinery and no
advantage. Speed is not in this table and is not favourable: `-9` runs at 0.3–0.6 MB/s here
against `xz`'s 1–2 MB/s.

## 4. Container payloads: what the nested transforms actually do

A container payload puts its own header in front of the member bytes, so every nested transform
now scans for its signature **anywhere** and carries a prefix and a tail. All of them are
multi-image: every image in the payload is transformed, not just the first.

| container | transform | measured | fixture quality |
|---|---|---|---|
| ZIP of **BMPs** | plane split + row delta | **−36.9% / −48.3%** | photographic, verified |
| ZIP of **PNGs** | inflate the PNG's DEFLATE | −79.2% ⚠️ | synthetic, **pending** |
| ZIP of **GIFs** | decode LZW to indices | −69.6% ⚠️ | synthetic, **pending** |
| ZIP of **JPEGs** | brunsli on every image | −16.5% ⚠️ | synthetic, **pending** |

The BMP row is the only one measured on real content. The others are known to be optimistic.

## 5. Measured and rejected — do not rebuild these

| idea | result | why |
|---|---|---|
| LZ4 precompression by storing the parse | **+50%** worse | the recipe costs what LZ4's own descriptors cost |
| "zip store" unpacking | **−1.4%** | the container's redundancy is already collected |
| split archives compressed per part | **~2× worse** | belongs in the orchestrator, not the codec |
| parallelising the selector | no gain | the cost is two backends, not the fan-out |
| Tier-1 fallback on a second thread | **9% slower** | 2 cores, 2 GiB; the working sets do not fit |
| adaptive Huffman | **22% of the run, never smaller** | turned off by default; `--huff-adapt` restores it |
| base64 of single-line structured text | **−6.3%** (a loss) | the gate declines it |
| near-duplicate delta stage | not needed | the match finder already handles it |
| lrzip/rzip for the srep step | nothing to add | `dedup` is within 0.1% of the ideal |
| oxipng/optipng before archiving | 3 bytes on a 66%-smaller PNG | the transform stores pixels, not deflate choices |
| moving the precomp gate after the nested stages | identical output | the gate was never the blocker |

## 6. The audit

`compressor.cpp` and `decompressor.cpp` were read end to end. **Eight findings, six fixed.**

| # | finding | status |
|---|---|---|
| 1 | the stage order is not a derivation, and nothing said so | documented in the code |
| 2 | `DecompressReport::original_size` lied for transformed streams | fixed, tested |
| 3 | "the default runs no Layer 0" was false, and written down three times | corrected; the default is now a test |
| 4 | the inverse chain had **no expansion bound** | fixed by `true_size` in v6 |
| 5 | nothing checked a block's declared size against its body | `write_block` guard, tested |
| 6 | `ecc_unpack` allocated **2352×** a declared sector count | structural bound, tested |
| 7 | **data race** on `stats.tokens` between block workers | per-block accumulation, tested |
| 8 | `dedup_unpack` trusted a 1 TiB declared size | bounded reserve, tested |

The three that would have done real damage (4, 6, 7) were invisible to a suite that was green
throughout. **Not audited, and not claimed**: the vendored entropy coders, the transforms'
internals beyond their contracts, `main.cpp`, the match finder and parser.

## 7. What is open

- **Re-measure PNG and GIF on real content.** The only remaining figures known to be optimistic.
  Everything else in this document has been verified or corrected.
- **`precomp_helps` accuracy.** Its structural rule (payload ≥ 2× original → accept) can take a
  payload that encodes worse; a Tier-0 guard now catches that case, but the rule itself is still
  a predictor.
- **The base64 gate costs ~2.7× compress time** where it runs.
- **Speed.** 0.3–0.6 MB/s is the trade for the ratio, and it has not been attacked.
- **The 99 MB ISO and 128 MB raw image** are unmeasured: ~8–10 minutes each at these speeds.
- **LZ4 salvage** by re-encode-and-compare — untried, and the only route that could still pay.

## 8. How to read the rest of this directory

The per-item documents are the working record: the measurement, the attempt, the bug, the
correction. Several were revised in place and say so at the bottom. If you want the current
number for anything, it is in this document. If you want to know *how* a number was arrived at,
or what went wrong on the way, the per-item document has it.
