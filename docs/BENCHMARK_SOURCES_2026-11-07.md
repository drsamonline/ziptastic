# The 2026-11-07 link list, read

Twenty-four links arrived on 2026-11-07: papers, benchmark sites, and a dozen repositories. This is
the verdict on each — what it is, whether anything in it can be used, and what it changes about how
this project positions itself. The short version: **nothing from this list should be vendored**, and
two things from it change what this project should measure next.

## What the list is actually about

Fifteen of the twenty-four links are **Large Text Compression Benchmark and Hutter Prize entries** —
the enwik9 leaderboard. That is a different sport from this engine's. LTCB scores
`compressed enwik9 + a zip of the decompressor`, on one 1 GB XML file, with no time limit. ziptastic
compresses arbitrary trees at 0.4 MB/s and refuses to be slower than that; the frontier here runs for
hours or days per gigabyte and several entries require a GPU.

Useful to know, and worth stating plainly rather than implying we compete: **the 2026 frontier is
around S ≈ 95.8 MB for enwik9, about 0.77 bits per byte**, and every entry at that level is a
transformer-augmented context-mixing model.

| entry | enwik9 S | notes |
|---|---|---|
| `makimono` (krapuchino83) | **95,793,872** | self-extracting, modified zmix 1.0, written in **Zig** |
| `cmix-lex-transformer` (Neel49) | 99,313,750 | cmix with a lexical transformer, GPL-3 |
| `cmix-neif-1.0` / `Ex-Girlfriend` | ~100.0 MB | Wölk's 2026 entry and Landay's verified derivative |
| `fx2-cmix-T-ft8` (mejutoco) | 100,367,271 | transformer weights, 8 further QAT epochs |
| `RATA-CMIX` | CPU-only candidate | built on `fx4-cmix-transformer` |
| NNCP (Bellard) | 106,632,363 (0.853 bpb) | Transformer in C over LibNC; enwik8 14,915,298 |
| `tufazip`, `altxs` | Python drivers | `tufazip` needs **two NVIDIA B200s**; `altxs` is a harness for the same benchmark |

This project's own numbers are on ordinary content, against `xz` — `FINDINGS.md` §3. Nothing here
changes them, and no result in the table above is comparable to them.

## Per-link verdict

| link | what it is | use it? |
|---|---|---|
| researchgate 371054542 | a general "lossless compression" survey | no — nothing measured, nothing new |
| arXiv 2308.12275 | 22 file formats over 178 GB, **zlib only** | confirmation, not news: video/audio/image barely compress, text does. The per-type dispatch this engine already has. |
| CAST white paper | vendor marketing for compression IP cores | no |
| arXiv 2606.17712 | not retrievable — see below | — |
| encode.su forums | the community where most of these tools are discussed | already mined: `docs/ENCODE_SU_SOURCES.md` |
| **TurboBench** | benchmark harness, many codecs, **no licence file** | no — no licence means no vendoring, and we have `tests/benchmark/` |
| **lzbench-web** | browser front end for lzbench results | as a sanity check only |
| **PeaZip max-compression benchmark** | 303 MB corpus, six tools, ratio and time | **yes, as a reference row** — see below |
| **mattmahoney.net/dc/** | LTCB, plus the largest collection of compressor sources and links anywhere | **already mined** — see below |
| **squash** | MIT compression abstraction library: one API over many codecs | no — this project's plugin host already does this, and `squash` is a *front end*, not a codec |
| **libarchive** | BSD archive/compression library | no — but it is the right reference for what "handles every container" costs |
| web.archive maximumcompression.com | the old maximumcompression tables, archived | partially — the numbers are 2005-era and superseded by LTCB |
| ex-girlfriend, cmix-neif, makimono, cmix-lex-transformer, RATA-CMIX, fx2-cmix-T-ft8 | Hutter Prize entries | **no — all GPL-3 or unlicensed**, and all aimed at one file |
| freement.cloud/hutter | fractal image compression write-up | no — lossy-adjacent, and out of scope for a byte-exact archiver |
| tufazip, altxs | Python drivers around the same benchmark | no |
| **nncp** (bellard.org) | Transformer-based compressor, C + LibNC, published results | **read the papers, do not vendor**: GPL-2, and its lesson for this project is about model size, not about code |

## The two things that matter

**1. We have no standard corpus, and that is why we cannot cite published numbers.** Every table in
this list is on Calgary, Canterbury, Silesia, enwik8 or enwik9. This project's tables are on its own
docs, a source tree and a synthetic asset tree — honest, reproducible, and impossible to compare with
anyone else's. Getting Silesia (211 MB, twelve files) would let `FINDINGS.md` carry a row that a
reader can check against PeaZip, lzbench and TurboBench directly. That is the cheapest credibility
this project could buy, and it is a download plus a script, not a research programme.

For scale, PeaZip's table on 303 MB of exactly those standard corpora:

| tool | size | ratio | compress | decompress |
|---|---|---|---|---|
| ZPAQ ultra | 57.6 MB | **19.01%** | 359 s | **358 s** |
| 7-Zip ultra (LZMA2) | 71.2 MB | 23.50% | 137 s | 3.4 s |
| 7-Zip ultra + Brotli | 75.1 MB | 24.79% | 208 s | 0.8 s |
| 7-Zip ultra + Zstd | 75.3 MB | 24.85% | 300 s | 1.2 s |
| RAR best | 78.1 MB | 25.78% | 28.5 s | 1.8 s |
| 7-Zip ultra + BZip2 | 80.6 MB | 26.60% | 81 s | 7.1 s |

Read the last two columns before the ratio: ZPAQ wins by 4.5 points and costs **105× the decompression
time**. That is the trade this engine lives in, stated by someone else's measurement.

**2. The mattmahoney.net collection is already mined, and the answer was no.** `vendor/hcs/old/` holds
58 sources including `LPAQ9`, `FPAQF2`, `SR3A`, `paq8px`, `zpaq-master`, `zpaqfranz`, `omnizip-zpaq`,
`BBB` and `nanozip` — the CM and LZ families from exactly that page, with licences recorded in
`vendor/hcs/SOURCES`. Nothing else on the page is a better fit than what is already there, and adding
more CM variants would grow a vendor tree that already takes most of the repository.

## One link that did not resolve

`arXiv 2606.17712` does not exist: the identifier is a **June 2026** submission number and the
listing is empty. Either a typo or a paper that was withdrawn. Worth re-sending if you have it.

## So: what changed

Nothing in the vendor tree. Two things on the work list:

- **Silesia, and a published-benchmark row in `FINDINGS.md`.** The one measurement that would let
  anyone else check this project's claims against a table they already know.
- **Positioning, stated once and not repeated**: LTCB is a different benchmark on a different corpus
  with no time limit, and the 2026 frontier there is transformer CM at ~0.77 bpb. This engine is not
  in that race and should not pretend to be; it is in the race ZPAQ is in, where 19.01% of a standard
  corpus costs 358 seconds to unpack.
