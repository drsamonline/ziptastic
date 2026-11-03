# Two tool suggestions, measured: lrzip for srep, and the PNG optimisers

Written 2026-11-03, in answer to "can lrzip replace srep" and the oxipng/optipng/
browser-image-compression suggestions.

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
