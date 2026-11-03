# Against the field: ziptastic vs zstd -19

Written 2026-11-03. The project has spent a year measuring itself against itself. This is
the first table with an outside reference in it.

## Result

| file | ziptastic `-9` | zstd `-19` | difference |
|---|---|---|---|
| 2 MB English prose | 408,418 | 546,717 | **−25.3%** |
| 6.8 MB spreadsheet sheet XML | **70,086** | 390,673 | **−82.1%** |
| 4.8 MB cooked ISO image | 3,333,900 | 3,352,732 | **−0.6%** |
| 2 MB prose, `--max-ratio` | **376,746** | 546,717 | **−31.1%** |

Everything round-trips; every ziptastic number here came from a run in this repository's
own history.

## What the numbers say

**The gain is not uniform, and the pattern is the point.** Text gains 25%, structured XML
gains 82%, and an already-compressed disc image gains 0.6%.

That last row is the honest one. A cooked ISO is mostly compressed content and padding; no
general compressor can do much with it, and zstd is within 0.6% of us. **ziptastic is not
magic on incompressible data** — it is a set of format-aware transforms plus a strong
backend, and where there is no format and no redundancy to find, it is just another
compressor.

The XML row is where the architecture pays: the spreadsheet numeric transform alone is
worth −50.3% (`XML_NUMERIC_MEASUREMENT.md`), and zstd has no idea the file is a spreadsheet.

## Caveats, stated rather than buried

- **zstd is one reference, not the field.** `xz`/`7z` are not installed in this sandbox, so
  the LZMA-class comparison here is implicit: the engine's own Tier-1 guarantee means its
  output is never worse than its LZMA2 fallback, which is the xz-class baseline. A real
  head-to-head against `xz -9e` and `7z -mx9` is still owed.
- **Speed is not in this table**, and it is not favourable: `-9` runs at roughly 0.3–0.6
  MB/s here against zstd's tens of MB/s. zstd `-19` on the 2 MB file finishes in about a
  second; `--max-ratio` on the same file takes a minute.
- **These are three files.** A 2 MB synthetic prose file, a generated sheet XML, and one
  small ISO. The corpus in `TEST_CORPUS.md` is the honest sample, and most of it is still
  unmeasured for time.

## Why it is worth recording anyway

Because "25% better than zstd -19 on text, 82% better on spreadsheets, parity on compressed
images" is a much more useful sentence than "the engine has seventeen transforms", and it
is the sentence a user deciding whether to use this needs.

## Against `xz -9e` — the LZMA-class baseline, measured

`xz` is not installed in this sandbox, but **Python's `lzma` module is liblzma**, and
`lzma.compress(data, preset=9 | PRESET_EXTREME)` is `xz -9e` byte for byte. So the comparison
the previous section said was "still owed" is here.

| file | raw | ziptastic `-9` | `xz -9e` | difference |
|---|---|---|---|---|
| 2 MB English prose | 2,000,044 | **408,421** | 543,572 | **+24.9%** |
| 6.8 MB sheet XML | 6,858,489 | **70,090** | 163,656 | **+57.2%** |
| 4.7 MB raw CD image | 4,704,000 | **1,417,452** | 2,014,224 | **+29.6%** |
| 4.8 MB cooked ISO | 4,980,736 | **3,333,904** | 3,339,112 | **+0.2%** |
| 1.4 MB floppy image | 1,474,560 | 1,257,830 | **1,257,792** | −0.0% |
| 9 MB with a 3 MB duplicate at range | 9,000,000 | **6,000,175** | 6,001,708 | +0.0% |
| 8 MB asset-heavy near-duplicates | 8,000,000 | 228,012 | **227,972** | −0.0% |

## What this table actually says

**Where a format transform fires, the win is large and structural**: prose +24.9%, sheet XML
+57.2%, raw CD image +29.6% (that last one is ECM regenerating 276 bytes of parity per
sector). No general-purpose compressor can reach any of those, because none of them know what
the bytes are.

**Where nothing fires, it is a tie.** The cooked ISO (+0.2%), the floppy image (−0.0%), the
long-range duplicate file (+0.0%) and the near-duplicate asset tree (−0.02%) are all within
noise of `xz -9e`. On data with neither a format to parse nor redundancy a general compressor
misses, ziptastic is an LZMA-class compressor with more machinery and no advantage.

**Two files are marginally behind** — the floppy image by 38 bytes in 1.26 MB, and the
asset-heavy tree by 40 bytes in 228 KB. Both are 0.0% at any sane precision, and both are
worth stating rather than rounding away: the engine does not win every comparison, and the
places it does not win are the places it has nothing format-specific to offer.

**Speed is still not in this table and still not favourable.** `-9` runs at ~0.3–0.6 MB/s
here; `lzma` at preset 9e is roughly 1–2 MB/s on the same box, and `xz`'s own multithreaded
mode is far faster. The trade is ratio for time, deliberately, and `--max-ratio` trades more of
both.
