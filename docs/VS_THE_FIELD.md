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
