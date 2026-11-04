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

Format **v8**. Header: `"CHCE" | version | flags | layers | layers2 | layers3 | original_size |
true_size | block_count | crc32`, per block `mode | usize | csize | sha256[32] | metadata |
payload`, then a `ZTIX` trailer with the block count, a manifest digest, **one absolute u64 per
block** and a `ZTOX` tail marker.

- **`original_size`** is the size *after* Layer 0; **`true_size`** is what the caller gets back.
- Three layer bytes, because each was filled as it was needed. `layers3` has seven free bits.
- Nine backends per block (RAW, rANS, static Huffman, range, BWT, PPMd, LZMA2 fallback, CM under
  `--max-ratio`), chosen by smallest output. Adaptive Huffman is off by default.
- **The block offset table** (v8, Phase 3b): every block's position is in the trailer, so a
  block can be reached without walking the stream — 0.76% of the archive read to find any one
  block, and one block decoded in about 1/100th of a full decode. Section 3c.
- 34 test suites, all passing; 53 CLI assertions; plus `scripts/regression-corpus.sh`,
  which asserts the property each historical bug broke rather than only the round trip. A full audit of `compressor.cpp` and
  `decompressor.cpp` is recorded in section 6.

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
| PNG image data inflated | layers2 0x10 | **−7.0% / −10.6%** | photographic PNGs (re-measured) |
| **CD sector parity (ECM)** | layers2 0x20 | **−28.2%** | real Mode 2 Form 1 image |
| BMP colour planes + row delta | layers2 0x40 | **−36.9% / −48.3%** | photographic BMPs |
| PNG one level down | layers2 0x80 | **−0.3%, usually declines** | ZIP of photographic PNGs |
| **BMP one level down** | layers3 0x01 | **−36.9% / −48.3%** | ZIP of photographic BMPs |

**These were re-measured on real content on 2026-11-04, and the earlier synthetic figures were
2–8× too optimistic.** The original fixtures were generated gradients whose pixels are a formula;
they compress to almost nothing under any transform, so they measured the fixture rather than the
transform. BMP was corrected first (−99.2% → −37% to −48%), then PNG, GIF and JPEG were checked
the same way. Every number in the tables above is now from real content.

**One regression this exposed, now fixed**: a 1.2 KB UI-style PNG encoded to **336 bytes with the
transform and 309 without it** — the gate accepted a payload the block encoder then made worse,
because on a file that small the block framing is a large fraction of the total. Below 2 MiB the
encoder now also builds the untransformed stream and keeps whichever is smaller, so a transform
cannot make a small file bigger. That file is **309 bytes** today.

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

## 3b. A realistic game-asset tree, end to end

The tables above are single files. This is the shape the engine is actually for: a mixed tree of
textures, audio, CSV and logs, 6.3 MB unpacked, shipped the two ways a game ships assets.

| archive | ziptastic `-9` | `xz -9e` | `zstd -19` |
|---|---|---|---|
| `tree.tar` (6,328,320 B) | **1,275,048** | 1,983,636 (+35.7%) | 2,064,101 (+38.2%) |
| `tree.zip` (2,797,544 B, deflated) | **1,284,836** | 1,610,228 (+20.2%) | 1,616,603 (+20.5%) |

Both round-trip byte-exact. **On a realistic mixed tree the engine is 20–36% smaller than `xz
-9e`**, which is the number to quote: not the 57% on sheet XML and not the 0.2% on a cooked ISO,
but the 20–36% a real asset tree gets.

Two things in that table are worth noting:

- **The ZIP carries `layers = 0x40`** — the second container level fired, so precomp reached
  inside the archive and the nested transforms ran on the members. The tar carries `layers = 0`.
- ~~**A ZIP inside a plain tar is not reached.**~~ **Fixed, and the diagnosis was wrong.** The tar
  was never the obstacle. `scan_zip` read each member's compressed size from its *local header*,
  and a ZIP written to a pipe — which every tool produces when it cannot seek — leaves those
  fields zero and puts the real sizes in a data descriptor after the data. So the member was
  skipped, and the same file failed whether or not a tar was around it. The scan now reads the
  central directory, which always carries the real sizes, and a stray local-header signature
  inside compressed data no longer stops it (`break` → `continue`).

  Measured on a 320,316-byte streamed ZIP of two text members: **320,424 bytes before** (no
  transform, and larger than the input) and **183,286 after** — −42.8%. The same ZIP inside a
  327,680-byte tar: **321,197 → 183,454**, −44.0%. Both byte-exact.

## 3c. Addressable blocks (v8), and what the table costs

Format v8 adds a block offset table to the trailer: one absolute u64 per block, plus a `ZTOX`
marker that lets a reader compute where the table starts from the end of the stream. Blocks were
already self-contained in parallel mode and already carried their own SHA-256; nothing could find
one without walking from the front. `FORMAT_V8.md` is the design record.

Measured with `tests/benchmark/index_cost.cpp` on 3.68 MB of concatenated project documentation,
16 KiB blocks, **225 blocks**, 1,377,975-byte archive:

| | |
|---|---|
| reading the whole index | **0.04 ms**, touching **10,414 of 1,377,975 bytes (0.76%)** |
| decoding the whole stream, walking the headers | **810–817 ms** median |
| decoding the whole stream from the index, 2 threads | **800–842 ms** median |
| decoding one block from its offset | **4–11 ms** (two runs on a 2-core box) |

The table costs `8 × blocks + 4` bytes over a v7 trailer:

| case | archive | blocks | added | share |
|---|---|---|---|---|
| 3.68 MB of docs, 16 KiB blocks | 1,377,975 | 225 | 1,804 | 0.131% |
| 460 KB of docs, 64 KiB blocks | 145,164 | 8 | 68 | 0.047% |
| 460 KB of docs, default block size | 118,284 | 1 | 12 | 0.010% |
| 1 KB of text | 687 | 1 | 12 | **1.7%** |

**The last row is a real cost, not a rounding error**, and it is the same shape as the 1.2 KB PNG
in section 2: on a single-block archive the table is 1.7% of the output and says nothing the reader
could not derive. It is kept for now because removing it means a second trailer shape, and the flag
byte is full, so it would cost another header field. If small files ever matter more than
uniformity, that is the change to make.

What the table is **not** allowed to do is be wrong. Four checks, none of which decodes a payload:
the table's structure; that the entries tile the stream from the end of the file header to the
trailer with no gap; that the headers found at those offsets re-hash to the stored manifest; and
that a decoder which walked the blocks sees the same offsets. A corrupted table is a refusal, never
a wrong answer.

**The two decode paths cannot be told apart on this machine, and an earlier claim that they
could was a benchmark artefact.** The first version of `index_cost` timed the walking path
first and the index path second, so whichever ran first paid for cold pages and first touch of
the output buffer — and that alone produced a "4–6% win" which was then attributed to the
digest check. With both paths warmed before either is timed, the two sit at 810–817 ms and
800–842 ms across repeated invocations, with a **±40 ms spread between runs on two cores**.
Every difference this benchmark is trying to resolve is 6–30 ms, which is inside that spread.

So the honest summary is arithmetic rather than measurement: the table removes a 10 KB header
scan from an 800 ms decode, which is microseconds, and **an index-driven decode is not
measurably faster than a walk when the stream is already in memory**.

The digest check did move into the workers in `decompress()`'s parallel path anyway
(`decode_blocks_parallel` already did it), because it is strictly less serial work and cannot
be slower — not because it was measured to help. Measured before and after with the warmed
benchmark: 809.9 vs 817.3 ms median, which is the noise.

`decompress()` was therefore **not** rewritten to decode from the index. A second decode path
in the same function, for a difference that cannot be resolved here, is a bad trade —
`FORMAT_V8.md` records it.

**And a partial decode of a transformed stream is refused, not answered** (`not-self-contained`):
an archive-mode block may reference an earlier one, and the inverse chain needs the whole stream,
so a block's plaintext is only the caller's bytes when the stream was written `--parallel` with no
Layer 0 transform.

## 3d. Block size, swept at last

`kDefaultBlockSize` has been 4 MiB since the note in `format.hpp` that recorded one corpus
(dickens) at three sizes. Nothing had swept it since, and `RELEASE_PLAN.md` Phase 3 listed it as an
open measurement. Swept 2026-11-04 on four real corpora, native block loop (`-9 --no-lzma2`, so the
whole-stream fallback cannot mask the result), archive size in bytes:

| corpus | 64 KiB | 256 KiB | 1 MiB | 4 MiB | 16 MiB |
|---|---|---|---|---|---|
| `tree.tar`, 1.6 MB mixed | 375,442 | 346,417 | 322,462 | **316,900** | 316,900 |
| `code.txt`, 846 KB | 173,654 | 159,520 | **149,468** | 149,468 | 149,468 |
| `big.txt`, 1.4 MB vendor code | — | 187,730 | 185,179 | **180,016** | — |
| `huge.txt`, 2.75 MB mixed sources | — | — | **393,003** | 395,411 | 395,411 |

Cells repeat where the block size exceeds the file: one block either way is the same stream.

**4 MiB stays.** Against 256 KiB it is 4.3–9.3% smaller on the homogeneous corpora, and against
1 MiB it is between 1.8% smaller and **0.6% larger**. The one corpus where a smaller block wins is
the heterogeneous one — 2.75 MB of unrelated source files — because the per-block selector can pick a
different backend for each block, and a 4 MiB block forces one choice across content that does not
share a model. So there is no single best block size: it trades model size against the selector's
freedom to adapt, and the trade moves with how uniform the input is.

4 MiB is within 0.6% of the best on every corpus measured and is the size every other number in this
document was taken at, so it does not move without a reason that is bigger than 0.6%. **An adaptive
block size** — split smaller when the first block's chosen mode differs from the second's — is the
obvious next idea and is not attempted.

Speed is not in this table because this machine cannot resolve it (§3c): every size ran at
0.37–0.45 MB/s on these corpora, which is the spread between runs, not between block sizes.

## 3e. The Huffman candidate is now gated by a measurement

`SELECTOR_PROFILE.md` measured the static Huffman candidate at **18% of a 1 MB prose run for
nothing** — byte-identical output with and without it — and said the next step was a sample trial
rather than a default flip. Built 2026-11-04.

The trial encodes a 64 KiB prefix of the block with the coder that would actually run, scales that
to the block, and runs the full candidate only if the estimate is within 1% of the best candidate so
far. A prefix because a byte histogram is a whole-block property and any slice represents it; the 1%
slack is deliberately in Huffman's favour, because running it needlessly costs time and skipping it
wrongly costs ratio.

**No ratio changed.** Archive sizes on all six corpora, gated against `--no-huffman`:

| corpus | gated | `--no-huffman` | outcome |
|---|---|---|---|
| `tree.tar`, 1.6 MB | 316,900 | 316,900 | skipped |
| `code.txt`, 846 KB | 149,468 | 149,468 | skipped |
| `big.txt`, 1.4 MB code | 180,016 | 180,016 | skipped |
| `huge.txt`, 2.75 MB mixed | 395,411 | 395,411 | skipped |
| `p1m.txt`, 310 KB text | 70,726 | 70,726 | skipped |
| **`seven.bin`, 1 MB, 7 symbols** | **287,758** | 289,862 | **ran, won 0.73%** |

The first five are byte-identical to the numbers in §3d, taken before the gate existed. The last one
is the case the candidate exists for, and the trial lets it run.

**The time saving is not claimed.** `SELECTOR_PROFILE.md`'s 18% was measured on a different file
with an older build, and this machine cannot resolve an effect that size against its own spread
(§3c). What is measured is the decision: `-v` now reports `huffman: N offered, M skipped by the
sample trial`, because the output is identical whether the candidate ran and lost or never ran —
a size comparison cannot tell you whether a gate is working.

## 3f. The LZMA2 fallback, gated with a 20% margin

The Tier-1 fallback is a guarantee, not an optimisation: it exists so a stream is never much worse
than an LZMA-class compressor. `SELECTOR_PROFILE.md` measured it at 34% of a run and said the fix
was a sample comparison that could only *trigger* the full run.

Gated 2026-11-04. A 64 KiB middle slice is encoded with the real LZMA2 encoder at the same settings,
scaled to the whole stream, and the full run happens unless that estimate is more than **20% worse**
than the native stream.

| corpus | trial | archive | ungated archive |
|---|---|---|---|
| `tree.tar`, 1.6 MB mixed | skipped | 316,900 | 316,900 |
| `code.txt`, 846 KB | skipped | 149,468 | 149,468 |
| `big.txt`, 1.4 MB code | skipped | 180,016 | 180,016 |
| `seven.bin`, 1 MB, 7 symbols | ran | 287,758 | 287,758 |
| `huge.txt`, 2.75 MB mixed | ran | 395,411 | 395,411 |
| `p1m.txt`, 310 KB text | ran (below the trial threshold) | 70,726 | 70,726 |

**No archive size changed.** On `tree.tar` the fallback cost 2.8 s of a 6.6 s run and won nothing;
with the gate that is **6.6 s → 4.1–4.8 s**, about a third, against 3.8 s with `--no-lzma2`.

Three things about the margin, because it is the whole safety argument:

- **20%, not 5%.** At 5% the gate also skipped everywhere it skipped at 20% on these corpora, and
  saved nothing extra — so the extra margin is free. It is the difference between "LZMA2 might just
  win" and "LZMA2 would have to win by more than a fifth".
- **The estimate is biased against LZMA2.** A 64 KiB slice starts with a cold dictionary, so it
  encodes worse than the whole stream would; the margin is spent correcting a bias that points the
  safe way.
- **The sample is 64 KiB because liblzma's setup, not its coding, is what the trial pays for.** At
  256 KiB the trial cost 1.4 s on a 1.6 MB archive — within a third of the run it was trying to
  avoid — which made the first version of this gate worthless. That was measured, not assumed.

**What this costs the promise**: the fallback is no longer unconditional. On a stream where the
sample says LZMA2 is more than 20% out of reach and the whole-stream run would have proved it wrong,
the archive is up to that much larger than it would have been. `--no-lzma2` is still the flag that
removes the candidate outright; `-v` reports `lzma2: N offered, M skipped`.

## 3g. The base64 gate: what it costs, and the one part of it that could be cheapened

`base64_pays` decides whether decoding base64 runs is worth it by running the **real pipeline** on
a 256 KiB sample of each side. Two earlier versions estimated instead, and both were wrong in the
direction that costs ratio (a JPEG missed by 19.6% because brunsli was not in the model; an XML case
backwards because the spreadsheet transform was not either). The expensive trial is the price of
being right.

Measured 2026-11-04, `-9`, on this box:

| file | default | `--no-base64` | gate cost | decision |
|---|---|---|---|---|
| 34 KB of base64'd text | 1.08 s | 0.52 s | 2.1× | taken, **8,667 vs 12,346 (−30%)** |
| 540 KB of base64'd random bytes | 11.91 s | 5.14 s | 2.3× | declined, archive identical |

**A decisive first pass is now taken, and only a decisive one.** A 64 KiB sample through the same
pipeline, one block per side, decides when the two sides differ by **20%** — three times the ~6% error
a 64 KiB sample is known to carry here (an earlier gate at that size flipped both XML cases by 5.8%).
Anything closer goes to the full 256 KiB trial.

- 34 KB base64'd text: **1.76 s → 1.08 s**, same archive, transform still taken.
- 540 KB base64'd random bytes: no change, because that case is a **tie** — and a tie is exactly what
  the cheap pass cannot decide and must not guess at.

So the saving is real on the files where the answer is obvious, and the dominant cost — resolving a
tie — is untouched. That is the honest shape of it: the remaining 2.3× is the price of not guessing,
and three failed estimates say the price is worth paying.

## 4. Container payloads: what the nested transforms actually do

A container payload puts its own header in front of the member bytes, so every nested transform
now scans for its signature **anywhere** and carries a prefix and a tail. All of them are
multi-image: every image in the payload is transformed, not just the first.

| container | transform | measured | verified on |
|---|---|---|---|
| ZIP of **BMPs** | plane split + row delta | **−36.9% / −48.3%** | photographic |
| ZIP of **JPEGs** | brunsli on every image | **−24.9%** | photographic |
| ZIP of **GIFs** | decode LZW to indices | **−11.1%** | photographic |
| ZIP of **PNGs** | inflate the PNG's DEFLATE | **−0.3%, often declines** | photographic |

Measured against the same container with the nested stage disabled, so the figure is what the
transform adds on top of precomp.

**The PNG nested case does not reproduce at all on real content.** It was −79.2% on a ZIP of
synthetic gradients and is −0.3% on a ZIP of photographic PNGs, where the gate usually declines
the payload outright: a photographic PNG's scanlines are not much more compressible than the
DEFLATE stream that already holds them. The transform is not wrong; the fixture was.

**JPEG is the surprise in the other direction**: −24.9% on real photographic JPEGs against −16.5%
on the synthetic ones, because brunsli's gain comes from the DCT coefficients and a real photo
has more of them worth modelling than a formula does.

Top-level GIF is worth mentioning separately because it is the strongest image result in the
engine: a photographic GIF goes 45,588 → **25,268** (−44.6%) and a 512×512 one 113,320 →
**58,036** (−48.8%), because LZW is a 1980s coder and the palette indices behind it compress far
better. That is a *file* result, not a container one.

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
| decoding from the offset table instead of walking, in memory | 810–817 vs 800–842 ms, inside a ±40 ms spread | the header scan it removes is 10 KB; nothing to measure |
| moving the block digest into the decode workers | 809.9 vs 817.3 ms median | strictly less serial work, and smaller than this machine's noise |
| the block offset table as a speed feature | nothing to speed up | it is an addressing feature; §3c |
| a cheap 64 KiB trial for the base64 gate, answering every case | flips both XML cases by 5.8% | only *decisive* answers are taken; a tie still pays for the 256 KiB trial; §3g |
| turning static Huffman off by default | no ratio change on five corpora, and it loses 0.73% on the seven-symbol file | gated by a sample trial instead; §3e |
| a 256 KiB trial for the LZMA2 fallback | 1.4 s of a 1.6 MB run, within a third of what it skips | liblzma's setup dominates, not its coding; 64 KiB and a 20% margin; §3f |

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

- ~~Re-measure PNG and GIF on real content.~~ **DONE** — see the note under the transform table.
  Every figure in this document is now from real content.
- ~~The PNG gate accepts payloads that encode worse on small files.~~ **FIXED** — under 2 MiB the
  encoder compares against the untransformed stream and keeps the smaller. The class of bug is
  general: every gate compares coded payload sizes at its own stage, and none of them could see
  the block framing that is added afterwards.
- **`precomp_helps` accuracy.** Its structural rule (payload ≥ 2× original → accept) can take a
  payload that encodes worse; a Tier-0 guard now catches that case, but the rule itself is still
  a predictor.
- **The base64 gate costs ~2.7× compress time** where it runs.
- **Speed.** 0.3–0.6 MB/s is the trade for the ratio, and it has not been attacked.
- **The 99 MB ISO and 128 MB raw image** are unmeasured: ~8–10 minutes each at these speeds.
- **LZ4 salvage** by re-encode-and-compare — untried, and the only route that could still pay.
- ~~The block offset table (Phase 3b).~~ **DONE** — format v8, section 3c. What it does not do is
  make archive mode addressable: that mode is sequential by construction, and `--block` on one is
  a refusal by design.
- ~~Nothing has been built on the offset table except the reader.~~ **Now also `decode_blocks`,
  `decode_blocks_parallel`, `--block` and `--from-block`.** The hatch pipeline is still heavenzip's
  and the shrinking extraction is HatchZip's.
- **Index-driven parallel decode is not a speed win in memory** (section 3c): 4–6%, and the digest
  check accounts for it. The gain the table actually offers is I/O — a reader that seeks — and this
  project's API takes a whole buffer, so that gain is unmeasurable here and untested.
- ~~**`decompress()` checks block digests after the join, on one thread.**~~ **Done** — the check
  now runs in the worker that decoded the block. Unmeasurable here (±40 ms spread), kept because it
  cannot be slower.
- **This machine cannot resolve a few percent.** Two cores, ±40 ms per run on an 800 ms decode.
  Any Phase 3 claim under ~5% needs a quieter box or many more runs before it means anything.
- **The 1.7% table cost on a single-block small archive** (section 3c) is unaddressed.
- ~~A ZIP inside a plain tar is not reached.~~ **DONE, and it was never the tar** — see §3b.
- ~~`precomp_helps`' 2× rule decides alone.~~ **Stated.** The rule is a predictor; under 2 MiB the
  Tier-0 guard catches a bad decision, above it nothing does, and the code now says exactly that
  where the line is. Making it measure means a full encode of both streams at any size.
- ~~The `layers3` free bits need a policy.~~ **DONE** — `FORMAT.md`, "Policy: who may take a free
  layer bit": four conditions for allocating one, and the four things that force a version bump
  instead.
- ~~A regression corpus.~~ **DONE** — `scripts/regression-corpus.sh`, run by `ctest` as
  `integration.corpus`: the streamed ZIP, the tar holding it, the small PNG (asserting the
  transform cannot make it bigger) and the near-duplicate tree, each asserting the property that
  was broken.

## 8. How to read the rest of this directory

The per-item documents are the working record: the measurement, the attempt, the bug, the
correction. Several were revised in place and say so at the bottom. If you want the current
number for anything, it is in this document. If you want to know *how* a number was arrived at,
or what went wrong on the way, the per-item document has it.
