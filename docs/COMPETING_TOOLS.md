> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# Two tool suggestions, measured: lrzip for srep, and the PNG optimisers

Written 2026-11-03, in answer to "can lrzip replace srep" and the oxipng/optipng/
browser-image-compression suggestions.

## 0. Where this engine does not compete, said once

The Large Text Compression Benchmark and the Hutter Prize rank `compressed enwik9 + a zip of the
decompressor` on one 1 GB XML file with no time limit. The 2026 frontier there is **S ≈ 95.8 MB, about
0.77 bits per byte**, and every entry at that level is a transformer-augmented context-mixing model
(`docs/BENCHMARK_SOURCES_2026-11-07.md` has the table and the licences).

This engine is not in that race and should not be presented as if it were. It is in ZPAQ's race: on
303 MB of the standard corpora, ZPAQ ultra reaches 19.01% and takes **358 seconds to unpack**, against
7-Zip ultra at 23.50% and 3.4 seconds. Ratio bought with decompression time is the trade this project
lives in, and the honest comparison is against tools that make the same one.

## 1. lrzip / lrzip-next for the srep step — we already do that job, to 0.1% of ideal

`lrzip` is rzip (long-range redundancy reduction) plus LZMA/zpaq. `srep` is long-range
redundancy reduction alone, used as a preprocessor before 7z/arc. The pipeline step in
question is the long-range matcher, and ziptastic has one: the `dedup` transform
(`kFlagDedup`).

Measured on the two shapes that matter, against the theoretical ideal — a file whose
duplicate content is removed exactly:

| test | size | unique content (ideal) | ziptastic `-9` | zstd `-19` |
|---|---|---|---|---|
| one 3 MB chunk repeated at 4 MB distance | 9,000,000 | 6,000,000 | **6,000,171** | 6,000,537 |
| 200 copies of a 20 KB asset spread over 9 MB | 9,000,000 | 5,020,000 | **5,024,405** | 5,025,625 |

Both land **within 0.1% of the ideal**, and both fired `dedup` (`flags 0x21` and `0x29`).
The 4 MB-distance case is the one that would expose a short window; the 200-copies case is
the game-tree shape. Neither leaves anything for rzip to find.

So the honest answer is **no, lrzip would not replace srep here** — not because rzip is
worse, but because the job is already being done and there is no headroom left in these
tests to measure a difference.

**What could still be worth taking from lrzip is not the rzip matcher but two things it
does that we do not:**

1. **Windows beyond ours.** rzip is built for multi-gigabyte ranges. Our dedup has a bounded
   window; a 128 MB disc image with duplicates at opposite ends has not been tested here.
2. **Near-duplicates.** Both tests above are *exact* duplicates. Real game trees are full of
   files that differ in a header or a few bytes, and that is a delta problem (srep handles
   it, rzip does not) rather than a matching problem. If anything is missing from our dedup,
   it is this, and lrzip is not the tool that provides it.

## 2. oxipng / optipng — they optimise the file, not the archive

Both re-compress a PNG by searching filter and deflate combinations. Measured directly:

| file | PNG size | archive size | `layers2` |
|---|---|---|---|
| `orig.png` (fast zlib, no optimisation) | 4,475 | **323** | 0x10 (PNG transform) |
| `opt.png` (optimised, the oxipng shape) | **1,503** | **326** | 0x10 |

The optimiser makes the PNG **66% smaller** and the archive **3 bytes bigger**. That is the
whole answer: ziptastic's PNG transform inflates the deflate stream and stores the
**pixels**, so the PNG's filter and zlib choices are thrown away and rebuilt exactly at
decode. Optimising them first changes nothing the archive can use.

They are not useless — they are for a different job. If the PNG is going to be *shipped or
extracted standalone*, a 66% smaller file matters. Inside an archive built by this engine,
it does not.

**browser-image-compression** is a different case again: it re-encodes with **quality loss**
(JPEG/WebP at a chosen quality, resizing). That is a product decision about what image you
want to keep, not a compression transform, and it cannot be part of a lossless archiver.
If the goal is "make the game's textures smaller", it is the right tool; if the goal is
"store this exact data smaller", it is not applicable.

## Summary

- **lrzip**: the srep job is already done to within 0.1% of ideal on both shapes measured.
  The open questions are window size and near-duplicates, and rzip answers only the first.
- **oxipng / optipng**: no archive benefit (3 bytes on a 66% smaller PNG); real benefit only
  for files shipped outside the archive.
- **browser-image-compression**: lossy, so a different product decision entirely.

## Follow-up: the near-duplicate gap, measured — and it is not a gap

The section above said the one thing possibly missing from `dedup` is **near-duplicates**
(files that differ by a header or a few bytes), because both tests used *exact* copies.
Tested it.

| test | size | ideal | ziptastic `-9` | zstd `-19` | `flags` |
|---|---|---|---|---|---|
| 200 near-copies of a 20 KB asset in a 9 MB tree | 9,000,000 | 5,020,000 | 5,103,853 (+1.7%) | **5,048,962** (+0.6%) | 0x01 |
| 40 near-copies of a 200 KB asset, 8 MB | 8,000,000 | ≈230,000 | **228,008** | 231,641 | 0x01 |

`flags = 0x01` in both: **`dedup` did not fire**, because the copies are not exact.

Two things fall out, and they point the same way:

1. In the **mixed** case zstd beats us by 1.1%, because its LZ77 finds the long identical
   runs *inside* each near-copy and our `dedup` skipped them entirely.
2. In the **asset-heavy** case we beat zstd by 1.6% (228,008 against 231,641) with `dedup`
   still not firing — the backend's own match finder found the near-duplicates.

So the near-duplicate work is **already being done, by the backend rather than by a delta
stage**. A dedicated delta transform would be duplicating a job the match finder does, and
the one case where we trail zstd (mixed content, 1.1%) is a case where a delta stage would
have to beat the match finder on data that is 98% random.

**Conclusion: not built, and not worth building.** The gap that looked like the missing
piece of the srep story turned out to be a measurement artefact of testing only exact
copies. Recorded because the instinct — "srep handles near-duplicates, so we need a delta
stage" — was reasonable and wrong, and the next person will have it too.
