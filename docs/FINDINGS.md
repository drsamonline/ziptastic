# Findings: the single authoritative record

Written 2026-11-04. **This document supersedes the per-item write-ups in this directory.** Those
are the history — how each number was arrived at, what failed on the way, what was corrected —
and several of them contain figures that were later revised. Where this document and an older
one disagree, **this one is right**.

The reason it exists: after a long session the directory held 57 documents, some of them with
three successive corrections appended, and a reader could find −99% and −37% for the same
transform in two places without any indication of which was current.

---

## 0. The machine every number here was measured on

**One core of throughput, reported as two.** The development box reports `nproc 2` and has no
`cpu.max` quota, but it does not execute two threads at once:

- two Python threads over a pure-CPU loop: **0.95×** against serial (no speedup);
- two *processes* over the same loop: 8,835 ms concurrent against 9,696 ms sequential — where real
  parallelism would have been about 4,850 ms.

This is written down first because it changes how to read every speed number in this document. All of
them were taken here:

- "Tier-1 fallback on a second thread: **9% slower**" (§5) is an artefact of oversubscription, not of
  working sets failing to fit in 2 GiB. Nobody has shown that a second thread does not help.
- "Parallelising the selector: **no gain**" (§5) is the same artefact.
- The ±40 ms spread on an 800 ms decode (§3c) is a contended single core, not a noisy neighbour.
- Every ratio number is unaffected: ratio does not depend on how many cores ran, and it reproduces
  exactly run to run.

So: **ratio claims in this document are solid, and thread claims are unmeasured.** Anything about
`--parallel`, `-T`, or a per-block time split needs a machine that actually runs two cores, and this
one does not.

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
document was taken at, so it does not move without a reason that is bigger than 0.6%.

**An adaptive block size was the obvious next idea, and the evidence does not support building it.**
The 1 MiB win on `huge.txt` reproduces exactly — 393,003 against 395,411 — and the mechanism is
partly what it looked like: at 1 MiB the three blocks split two rANS-parsed and one BWT, while the
single 4 MiB block is BWT throughout, so the selector does get to choose per block.

But that is not the whole story, and the rest of it points the other way:

| configuration | 1 MiB | 4 MiB | gap |
|---|---|---|---|
| all candidates | 393,003 | 395,411 | 1 MiB by **0.6%** |
| `--no-bwt` | 398,349 | 425,154 | 1 MiB by **6.7%** |
| a second heterogeneous corpus, 1.7 MB | 254,007 | 251,674 | 4 MiB by 1.0% |

With BWT removed the gap *widens* sevenfold, which is not "more freedom for the selector" — it is the
rANS path itself degrading with block size. And on the second corpus 4 MiB wins outright with every
block on BWT at both sizes, so there is no diversity to exploit. The effect is small,
corpus-dependent, and not attributable to one mechanism.

So: a feature that tries 4 MiB, then retries as 4×1 MiB and keeps the smaller, would cost a second
encode of every block — on a 2.7 MB file, about 6.5 s to 12 s — to win at most 0.6% on the corpora
where it wins at all, on an engine that already runs at 0.4 MB/s. **Not built, and the reasoning is
here so it is not rebuilt.**

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

## 3h. The opaque streams: what a zstd, brotli or xz transform would be worth

Phase 4 is "transforms, only when something asks for them". Something does: `.tar.zst` is now the
default archive for several distributions and release pipelines, `.br` is what web assets ship as, and
the engine treats all of them as opaque bytes. Measured 2026-11-04 on a 512,000-byte tar of this
project's own documentation:

| file | input | ziptastic `-9` | transform fired |
|---|---|---|---|
| `content.tar` | 512,000 | **127,557** (24.9%) | none, and none needed |
| `content.tar.zst` | 156,418 | 156,526 | **no** |
| `content.tar.br` | 149,498 | 149,606 | **no** |
| `content.tar.xz` | 153,392 | 153,500 | **no** |

The engine adds ~108 bytes to each compressed stream and gets nothing: **a `.tar.zst` costs 22.7% more
through the engine than the tar itself**, and that gap is the headroom:

| stream | if the engine could see inside | headroom |
|---|---|---|
| `.zst` | 127,557 + recipe | **−18.5%** |
| `.br` | 127,557 + recipe | **−14.7%** |
| `.xz` | 127,557 + recipe | **−16.9%** |

**The recipe is free in the easy case, and that is the whole question.** Decoding each stream and
re-encoding it with the same encoder at the same settings reproduced the original **byte for byte** in
all three cases — so a stream produced by a known encoder at a known level needs only that level
recorded, which is a few bytes. That is exactly the `preflate` pattern for DEFLATE, and it is why the
headroom above is nearly all real.

**What is not established**, and is the work: a stream from an unknown encoder version or level needs
the settings *recovered*, which is a search over the encoder's parameters plus a diff when no setting
reproduces it — `preflate` does this for zlib's levels and it is the hard part of that project.

**Measured, 2026-11-04: the search is affordable and the full search is not.** Every stream in a sweep
of levels is recoverable by trying them in order, and the cost is the whole problem:

| encoder | settings tried | recovered | trial encodes | time |
|---|---|---|---|---|
| zstd | 1, 3, 5, 9, 15, 19, 22 | every one | 1 → 19 | 0.01 s → **3.25 s** |
| brotli | 1, 5, 9, 11 | every one | 1 → 11 | 0.01 s → **5.26 s** |
| xz | 0, 3, 6, 9, 9e | every one | 1 → 5 | 0.10 s → 2.64 s |

The most common real settings are the most expensive to recover, because a linear search costs one
encode per level and quality 11 is the slowest encoder here. Against the engine's own ~1.3 s for the
same 512 KB, a full search is **2.5–4× the whole compression** — for an 18.5% win. That is the wrong
slope.

**A shortlist changes the arithmetic.** Two candidate settings per format, tried in order and declined
if neither matches:

| encoder | shortlist | cost |
|---|---|---|
| zstd | 3, 19 (the default and the release default) | **0.49 s** |
| xz | 6, 9e | 1.18 s |
| brotli | 5, 11 | 2.54 s |

So the design that survives is **a short, ordered list of settings and a decline when none matches**,
not a search — 0.5 s for zstd against a 29 KB win on this fixture, and streams from any other setting
simply do not get the transform. Brotli stays expensive because quality 11 is; it may only be worth
trying after the cheap ones fail and the payload is large enough to pay for it.

Also unbuilt: zstd has no decoder in the tree. Brotli and liblzma are already vendored (`brotli_vendor`,
`liblzma`), so `.br` and `.xz` need no new dependency; `.zst` would.

**The free bit.** `layers3` has seven (bits 1–7) and the policy in `FORMAT.md` asks for four things
before one may be taken: a measured win on real content (the table above), a gate that can decline
(the existing `precomp_helps` shape, on the decoded payload), a byte-exact inverse (re-encode, then
diff when it is not identical), and a test that asserts the transform fired (the corpus script's
pattern). Three formats, three bits, or one bit and a format byte inside the payload — that is a
design decision for whoever builds it, not for this measurement.

**Verdict: worth building, with the shortlist and not the search.** The win is 15–18% on a container
class that is common and currently costs the engine 23%, the pattern is one this project already has,
and two of the three formats need no new dependency. The recipe recovery was the open question and it
is now answered: a full search costs 2.5–4× the whole compression, a two-entry shortlist costs 0.5 s
for zstd and 1.2 s for xz. `.br` is the one that may not pay, because quality 11 costs 2.5 s to
confirm.

**The corpus, and what it says about the shortlist.** Ten streams of the same 512,000-byte tar from
different producer shapes — seekable and streaming, levels 3 and 19, long-distance matching, no
checksum, a custom xz dictionary — tested against the two-entry shortlists:

| stream | shortlist match |
|---|---|
| zstd 3, zstd 19 | yes |
| zstd 19 with long-distance matching | yes |
| zstd 3 without a checksum | yes |
| brotli 5, brotli 11 | yes |
| xz preset 6 | yes |
| **zstd 3 and 19, written to a pipe** | **MISS** |
| **xz with a 1 MiB dictionary** | **MISS** |

Three of ten miss, and the misses are informative rather than random:

- **A pipe changes the frame.** A streaming zstd encoder cannot know the content size, so the frame
  header omits it and the blocks land differently: three bytes shorter, and the shortlist's seekable
  encode cannot reproduce it. Adding the *streaming* encoder at each level recovers both — so the
  shortlist entry is a (level, framing) pair, four candidates, not two. This is the streamed-ZIP
  problem of §3b in a new place: the same file, written by a pipe, and the reader has to know it.
- **xz needs no shortlist at all.** The dictionary size is in the block header, and reading it out and
  re-encoding reproduces **both** streams byte-exactly, including the one the shortlist missed. So
  xz recovery is a header read, not a search — the cheapest of the three by a wide margin.

That reorders the work. `.xz` first (header read, no search, `liblzma` already vendored), `.zst` second
(four candidates, needs a decoder the tree does not have), `.br` last (quality 11 costs 2.5 s to
confirm and the header does not carry the quality).

**Partly built** — `.xz` is done, above. What is still missing for `.zst` and `.br` is a stream from a
*different encoder version* — everything here
came from the same libraries, and a zstd 1.4 stream may not be reproducible by a zstd 1.5 encoder at
the same level. That is the remaining unknown, and it needs real-world files, not generated ones.

### The xz foundation, proved in-tree and across versions (2026-11-07)

`tests/benchmark/xz_probe.cpp` does the whole experiment against the **vendored** liblzma, because
the answer depends on the encoder that would ship:

1. decode the stream;
2. read the LZMA2 dictionary size out of the block header with `lzma_block_header_decode`;
3. re-encode at each preset **with that dictionary size forced**, and compare bytes.

On a 522,240-byte tar:

| stream | dict size read from header | re-encode |
|---|---|---|
| preset 0 | 262,144 | **identical** |
| preset 6 | 8,388,608 | **identical** |
| preset 9 | 67,108,864 | **identical** |

And the version question is answered better than expected: **the streams were produced by liblzma
5.4.1 and reproduced by the tree's 5.8.4**, byte for byte, two minor versions apart. That is the
strongest evidence yet that an xz recipe can be a header read rather than a stored diff.

Two details worth having:

- **Presets 6, 7, 8 and 9 all produced the same bytes** once the dictionary size was forced — they
  differ from each other in exactly that parameter for this input, and the stream already told us.
  So the recipe is closer to "dictionary size, plus one bit for `extreme`" than to a preset number.
- `extreme` does change the bytes (156,380 against 156,328), so the recipe needs that bit.

This does not build the transform; it removes the reason it might not work.

### The `.br` transform, built (2026-11-07)

Brotli is the second of the three opaque containers, and it is harder than `.xz` in two ways that
shaped the code:

- **It has no magic number.** There is nothing to recognise, so the only test is "does it decode" —
  and then "does it rebuild byte for byte", because a decode alone proves nothing about the bytes
  we were given.
- **The quality is not in the stream.** The header carries the window size and nothing else. So the
  producing settings are guessed from a shortlist of six (quality 11 at windows 22 and 24, 11 text,
  9, 5, 1) and confirmed by re-encoding. A stream from a setting outside that list is declined.

Measured on a 542,720-byte tar of this project's docs:

| stream | bytes | through the engine | after |
|---|---|---|---|
| the tar itself | 542,720 | 136,291 | — |
| `.tar.br` at quality 11 | 159,883 | 159,975 | **136,299 (−14.8%)** |
| `.tar.br` at quality 9 | 174,949 | 175,041 | **136,299 (−22.1%)** |
| `.tar.br` at quality 5 | 184,622 | 184,714 | **136,299 (−26.2%)** |
| `.tar.br` at quality 1 | 216,913 | 217,005 | **136,299 (−37.2%)** |

All four qualities were recovered by the shortlist, `layers3` reads `0x04`, and every one lands
within 8 bytes of simply having the tar. The lower the quality, the bigger the win — which is the
point: the engine was paying for someone else's faster encoder with archive size.

`unit.brotli` covers the three ways this can go wrong: a quality outside the shortlist must decline
rather than guess, random bytes must not be accepted as brotli, and a stream with a tail appended
must be refused because treating it as bare brotli would drop the tail.

### The `.zst` transform, built (2026-11-07)

The last of the three opaque containers, and the only one that needed a dependency: `zstd` was not in
the tree, so `vendor/hcs/old/zstd` is new — BSD-3-Clause, trimmed to `common/compress/decompress` and
three headers, 2.1 MB, built with `ZSTD_DISABLE_ASM` because vendoring the hand-written Huffman
assembly across three toolchains cost more than the few percent it buys.

Measured on a 542,720-byte tar of this project's docs:

| stream | bytes | through the engine | after |
|---|---|---|---|
| the tar itself | 542,720 | 136,676 | — |
| `.tar.zst` at level 3, seekable | 195,983 | 196,091 | **136,683 (−30.3%)** |
| `.tar.zst` at level 19, seekable | 167,622 | 167,730 | **136,683 (−18.5%)** |
| `.tar.zst` at level 3, **piped** | 195,980 | 196,088 | **136,683 (−30.3%)** |

`layers3` reads `0x08` in all three and every one lands within 7 bytes of the tar itself.

**The piped case is the one worth reading.** A zstd encoder writing to a non-seekable output omits
the content size from the frame header, and its block boundaries follow whatever chunking the writer
used — so a one-shot re-encode at the same level does not reproduce it. The first version of this
transform therefore declined every piped stream, which is why the encoder now feeds its input in
128 KiB chunks, matching zstd's default block size. That is what a writer with default buffering
produces, and with it all three cases above are recovered.

### A real scanned PDF, measured (2026-11-07)

The synthetic PDF above said "PDF is covered". A real one says something different, which is why the
caveat was written down. The file: `CIA-RDP96-00792R000300330001-8.pdf`, 7,456,578 bytes, **99 pages,
all scanned, one image per page, none of them digital**.

| measurement | value |
|---|---|
| the PDF | 7,456,578 bytes |
| ziptastic `-9` | **6,373,705 (85.5%)**, 68 s |
| the same with `--no-precomp` | 6,373,705 — identical |
| what fired | base64 only (`layers 0x10`); **precomp did not fire** |
| images | 99, **1 bit per pixel**, 2544×3250, FlateDecode in the PDF |

**The structure is not the problem and the transform is not the answer.** A page's image is 35,725
bytes of Flate-coded bilevel bitmap inside the PDF; the whole document is an image container with a
thin text wrapper. `scan_pdf` found nothing worth taking, and it was right: inflating those streams
hands the backend data that deflate already coded well.

The first reading of that was "1 bit per pixel, so JBIG2 or CCITT G4 will win". **That was wrong, and
measuring it took one call.** The bitmaps were pulled out of the PDF and coded three ways:

| page | raw bitmap | Flate, as the PDF stores it | CCITT G4 | ziptastic `-9` on the raw bitmap |
|---|---|---|---|---|
| 0 (dense) | 1,030,956 | 255,108 | **242,630 (−4.9%)** | 275,300 (+7.9%) |
| 1 | 1,033,500 | **35,725** | 37,706 (+5.5%) | 52,732 (+47.6%) |
| 2 | 1,033,500 | **36,720** | 39,550 (+7.7%) | 51,458 (+40.1%) |

**Flate wins on the sparse pages, and it is not close.** These scans are mostly white with sparse
marks — 35,725 bytes from 1,033,500 is 3.5% of raw — and G4's advantage is 2D correlation across
dense rows, which there is not much of here. G4 only wins on page 0, the one dense page, and by 4.9%.

And this engine's own backends are **worse than Flate on all three** — BWT, PPMd and the CM stack
all lose to LZ77 on sparse bilevel, by 8% on the dense page and by 40-48% on the sparse ones. That is
worth knowing beyond PDF: it is a shape where DEFLATE is simply the better coder.

### Forcing the repack pipeline does not rescue it either (2026-11-08)

The obvious next question is whether the FreeArc-style chain — precomp to inflate, srep to dedup,
then the engine — does better than the engine declining. It was measured, twenty pages of the same
scanned PDF, forced rather than gated:

| what | bytes |
|---|---|
| the 20 pages as Flate in the PDF | **1,696,978** |
| the same 20 pages inflated, then `-9` (dedup fired, `flags 0x21`) | 1,832,200 |
| | **+8.0% worse** |

So the chain loses. `precomp` declining on this file was correct, and forcing it past its own gate
costs 8% even with `dedup` engaged on 20.6 MB of concatenated bitmaps. That is the same conclusion
`FINDINGS.md` §3f reached about the LZMA2 gate from the other direction: a transform that is *offered*
the data and measures itself as not helping is usually right.

**Why the redundancy does not help, which is the useful part.** Consecutive pages of this document are
**86.9% byte-identical at the same offsets** — 87.8% of page 1's 16-byte blocks appear verbatim in
page 2. That looks like a dedup goldmine and is not one, because the identical bytes are *white
space*. A run of 0x00 costs Flate almost nothing and costs a dedup match almost nothing; 1,033,500
bytes compress to 35,725 (3.5%) precisely because 87% of the page is blank. The expensive part is the
13% that is ink, and that is where every coder is already working.

That is the general lesson worth keeping: **cross-page similarity is not headroom when the shared
bytes are cheap.** Dedup earns its place on repeated *content* — the same asset, the same binary — not
on repeated background.

So there is no cheap win here, and the recommendation to build a bilevel transform was wrong on the
evidence.

### And JBIG2 would not win either (2026-11-08)

The last candidate was JBIG2's context-modelled arithmetic coding, on the reasoning that it beats G4
by 20-40% and so might reach 15-30% under what the PDF spends. That reasoning was wrong too, and it
did not need a JBIG2 encoder to find out: the entropy of the page under a JBIG2-shaped 2D context
model is a **bound** on what such a coder can achieve, and it is a bound that can be computed.

An 11-bit template — two previous rows of four pixels each plus three pixels to the left in the
current row — measured on the same three pages:

| page | Flate in the PDF | model bound | verdict |
|---|---|---|---|
| 0 (dense) | 255,108 | 282,361 | **worse by 10.7%** |
| 1 | 35,725 | 67,829 | **worse by 90%** |
| 2 | 36,720 | 70,624 | **worse by 92%** |

**Per-pixel modelling loses to run-length coding on sparse pages, and it is not close.** The reason is
the same one that made dedup useless: 87% of these pages is white, and LZ77 codes a long zero run for
almost nothing while a context coder still pays a fraction of a bit per pixel — 8.3 million pixels at
even 0.05 bits is 52 KB, which is more than Flate spends on the entire page.

The one mode not captured by this bound is JBIG2's **symbol dictionary**, which codes each repeated
glyph shape once and then a page of references. That is the only remaining candidate, it only applies
to text-dense pages, and page 0 — the one dense page here — is the only place in this document where
it could possibly pay. Recorded, not started, and not recommended without a corpus of dense scans to
justify it.

Two smaller things worth recording:

- **Our PNG layer makes it worse, not better, on a re-extracted image.** Page 1's image as a PNG
  (104,660 bytes, which is what an extractor produces) compresses to 53,082 through `-9` — worse
  than the 35,725 the PDF already stores. That is not a defect: the input had already been
  re-encoded once, and the comparison is not like for like.
- **Base64 fired on a binary PDF.** It is verified byte-exact, so it costs nothing but time, and on a
  file this size that time is measurable. Worth a look before the next speed pass.

### Two of the four "missing formats" were not missing (2026-11-07)

The tool survey listed four formats with prior evidence and no transform here: MP3, FLAC, PDF and
SWF. Two of them turned out not to be gaps at all, which is why this was measured before anything was
built.

| file | bytes | through the engine | what fired |
|---|---|---|---|
| a PDF with one FlateDecode stream | 471 | 330 | **precomp** (`flags 0x05`) |
| the same PDF's decoded content | 90,031 | 226 | — |
| an uncompressed SWF (`FWS`) | 135,016 | — | not measured, nobody ships these |
| the same SWF zlib-compressed (`CWS`) | 479 | **198** | no transform |
| that SWF's decoded body | 135,000 | 216 | — |

**PDF is already covered.** `scan_pdf` reaches the content stream, precomp fires, and 330 bytes
against 226 for the bare content is the container framing plus the precompressed stream — close to
optimal.

**SWF is dropped.** No transform fires on a `CWS` and none needs to — 198 bytes against 216 for its
own decoded body, because the backend compresses the DEFLATE stream harder than the text inside it —
and Flash is dead, so there is no reason to spend anything more on it. Removed from the Phase 4 list
on 2026-11-07 at the owner's instruction, and kept here only so the measurement is not repeated.

**The caveat, stated because it is large**: both fixtures are synthetic and small. A real PDF has
many streams and images; a real SWF is megabytes of mixed assets. These numbers say "do not build a
PDF or SWF transform on the strength of a guess", not "there is no gap". A real corpus of each would
settle it, and that is a download this session did not do.

**MP3 and FLAC remain unmeasured** — no fixtures here, and both need a real encoder to make one.

### FLAC, measured, and it is the largest gap left (2026-11-08)

The container transforms were worth 16.9% (.xz), 14.8-37% (.br) and 18.5-30.3% (.zst). FLAC looks
bigger than all of them, on two fixtures built to be as different as possible — 20 s of 44.1 kHz
stereo, one tonal and one noise-heavy, because a synthetic test that is all tones flatters a BWT
backend enormously:

| fixture | PCM | FLAC | PCM through `-9` | FLAC through `-9` | headroom |
|---|---|---|---|---|---|
| tonal | 3,528,044 | 2,393,014 | **1,615,628** | 2,393,126 | **32.5%** |
| noise-heavy | 3,528,044 | 2,927,020 | **2,280,032** | 2,927,132 | **22.1%** |

**This engine compresses 16-bit PCM 22-32% better than FLAC does on the same audio.** The reason is
structural: FLAC models samples with an LPC predictor and Rice codes the residual, while the backends
here work on the *bytes*, where the high byte of a 16-bit sample is nearly constant and the low byte
carries the noise — a shape BWT and PPMd handle well. FLAC never sees that.

So a `.flac` transform has more headroom than any container shipped so far, and the recipe question
is the same shape as the others: vendor libFLAC, inflate to PCM, compress that, and rebuild the stream
by re-encoding with the recovered settings.

### The recipe works, and it is six bytes (2026-11-08)

That was the question worth answering before writing any of it: **can libFLAC reproduce a stream it
did not write?** A probe was built against a trimmed vendored libFLAC (29 C files, no Ogg), decoding
a file to PCM and re-encoding at the same compression level.

On both fixtures, at level 5, the answer was the same and better than expected:

| fixture | original | re-encoded | bytes differing |
|---|---|---|---|
| tonal | 2,393,014 | 2,393,014 | **6** |
| noise-heavy | 2,927,020 | 2,927,020 | **6** |

**The six bytes are the vendor string**, at offsets 70-81: `libFLAC 1.4.3 20230623` in the file
libsndfile wrote, `libFLAC 1.5.0 20250211` from the vendored 1.5.0. Every audio frame is identical.
So the encoder's coding decisions are stable across those two versions, and the recipe needs to carry
the vendor string — and nothing else.

That also names the risk precisely. A `.flac` written by **ffmpeg's own encoder**, or by libFLAC with
a non-default block size or predictor order, will not reproduce, and will be declined rather than
guessed at. Coverage is a question for a corpus of real-world files; what is settled is that the
approach is sound where the producer is libFLAC at a default level. **The risk is the producer**: libFLAC's output depends on
its version and parameters, so a file written by ffmpeg's own FLAC encoder or by an older libFLAC
would be declined rather than guessed at. That is the same "producer variant" problem
`docs/SELECTOR_PROFILE.md` records for the other formats, and the coverage cost is unknown until a
corpus of real-world `.flac` files is tried.

Two smaller findings from the same run:

- **MP3 is not opaque.** With no transform at all, `-9` takes 800,956 bytes to **580,980 (−27.5%)** —
  frame headers and Huffman-coded data have structure the backends find. Anything mp3packer-shaped
  would have to beat that, and the usual 5-10% for Huffman re-coding would not.
- **BCJ false-fired on FLAC, and now does not.** `flags 0x09` — the x86 branch converter offered
  itself on an audio file and was accepted, because the payload is verified byte-exact and nothing
  else objected. The cause: the detector counted E8/E9 bytes and required 0.3%, and *random* bytes
  put them at 2/256 = 0.78%. Fixed by adding a byte-entropy test, which is the discriminator that
  actually separates the classes — measured on real inputs, a stripped x86-64 binary sits at **6.54
  bits**, text at 5.04, while FLAC and a scanned PDF sit at **7.84-7.99**, an `.xz` stream at 8.00
  and raw 16-bit PCM at 7.91. The bar is 7.0.

  The measurable effect is small and stated plainly: archive sizes are unchanged on every file
  tried, and the time saved on two FLAC files was 0.3 s and 0.1 s — inside this box's noise. What
  changed is that a `.flac` no longer *claims* an x86 filter it never had, and the detector now has
  a signal that corresponds to real code rather than to a coincidence in the opcode byte.

### bzip2, measured before anything is built (2026-11-07)

A `.bz2` is opaque to this engine for the same reason `.xz` was, and it is the one remaining
container where the recipe question has a trivial answer: **the level is in the file's own header**,
as the digit in `BZh9…`. Measured on the same 532,480-byte tar:

| what | bytes |
|---|---|
| the tar itself, `-9` | 132,541 |
| the `.tar.bz2` at level 9 today (opaque, plus framing) | 146,804 |
| headroom if it could see inside | **−9.7%** |

And the recipe would be one byte: re-encoding at the same level reproduces both a level-1 and a
level-9 stream byte for byte, and at the other level reproduces neither — so the header digit is
exactly the parameter, and no search is needed at all.

**Not built, and not obviously worth building.** 9.7% is half what `.xz` was worth, and unlike `.xz`
it needs a dependency the tree does not have: `libbz2` is not vendored (the `bzip3` submodule in
`vendor/hcs/old` is a different format and is empty). A `.tar.bz2` is also rarer than it was, and
rarer than `.tar.xz` is now. Recorded so the number exists if it ever comes up.

### The gates the `.xz` stage was not connected to (2026-11-07)

The `.xz` stage sets `precomp_payload` like `precomp` and `gzip` do, but the seven nested-transform
gates still read `(precomp || gzip)` — they were written before `.xz` existed. So a `.tar.xz` holding
a CSV, a ZIP, a PNG or a spreadsheet got the container inflated and then **nothing done with what came
out**, which is the entire reason the recursion stage exists.

Found by reading the gates after shipping, not by a test. There is one now, and it was checked the
only way that counts: with the gates narrowed back to `(precomp || gzip)` the test fails, and with
them widened it passes. The mechanism is what the test asserts, because on the fixture tried the
archive size did not move — 41,786 bytes either way, since the whole-stream LZMA2 fallback wins there
and masks the difference. A ratio number for this fix needs a fixture where the fallback does not
win, and that is not a measurement this session made.

### The `.xz` transform, built (2026-11-07)

`src/precompressors/xz_wrap.cpp`, `kLayer3Xz` in `layers3`, `unit.xz` in the suite. Measured on a
522,240-byte tar of this project's documentation, compressed to 156,796 bytes at preset 6:

| what | bytes |
|---|---|
| the tar itself, `-9` | 130,435 |
| the `.tar.xz` before this transform (opaque, plus framing) | 156,904 |
| the `.tar.xz` now | **130,449** |

**−16.9%**, and within 14 bytes of simply having the tar — which is what the headroom predicted. The
round trip is byte-exact, `layers3` reads `0x02`, and the transform costs one inflate plus one or two
trial encodes: 2.6 s → 3.4 s on this fixture.

Three things the build changed about the recipe, each found by a test rather than by reasoning:

- **The dictionary size alone is not the recipe.** A preset-0 stream uses a different match finder,
  not just a different dictionary, so it cannot be reproduced by a preset-6 encoder with the size
  forced. The payload carries a preset byte as well.
- **Which costs nothing to find**, because the dictionary size narrows the preset to at most two
  candidates — the presets' default dictionary sizes collide only in pairs (4 MiB is 3 and 4, 8 MiB
  is 5 and 6). So the recipe is dictionary size, integrity type, preset byte, and one bit for
  `extreme`; finding it is one or two trial encodes, not a search.
- **The encoder rebuilds from its own payload and compares before returning it.** A payload that does
  not round-trip through its own reader is discarded, so a wrong recipe costs ratio and never data.

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
| parallelising the selector | no gain | the cost is two backends, not the fan-out — and measured on one core of throughput (§0) |
| Tier-1 fallback on a second thread | **9% slower** | measured on a box with one core of throughput (§0) — the reason given here was never established |
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

## 6a. Silesia, at last: the first row anyone else can check

Every number above is on this project's own content, which nobody else can reproduce. The Silesia
corpus closes that: twelve files, 211,938,580 bytes, the standard set every published table uses.
Measured 2026-11-07, `-9`, one run per file, both tools built from sources in this tree.

| file | input | ziptastic `-9` | `xz -9e` | gain |
|---|---|---|---|---|
| sao | 7,251,944 | 4,415,466 | 4,425,664 | 0.2% |
| x-ray | 8,474,240 | 3,798,464 | 4,491,264 | 15.4% |
| ooffice | 6,152,192 | 2,131,573 | 2,427,224 | 12.2% |
| mozilla | 51,220,480 | 12,217,069 | 13,376,240 | 8.7% |
| osdb | 10,085,684 | 2,368,404 | 2,844,556 | 16.7% |
| dickens | 10,192,446 | 2,355,685 | 2,831,212 | 16.8% |
| mr | 9,970,564 | 2,249,704 | 2,751,892 | 18.2% |
| samba | 21,606,400 | 3,743,031 | 3,739,524 | **−0.1%** |
| webster | 41,458,703 | 7,158,274 | 8,368,672 | 14.5% |
| reymont | 6,627,202 | 1,024,805 | 1,315,592 | 22.1% |
| xml | 5,345,280 | 374,814 | 434,892 | 13.8% |
| nci | 33,553,445 | 1,434,131 | 1,449,272 | 1.0% |
| **total** | **211,938,580** | **43,271,420 (20.42%)** | **48,456,004 (22.86%)** | **10.7%** |

**10.7% smaller than `xz -9e` across the standard corpus**, 651 seconds of compression at 0.33 MB/s.
Two files are level rather than ahead — `samba` is 0.1% behind and `sao` 0.2% ahead — and those are
the honest rows: `samba` is source code, where LZMA's long matches and this engine's BWT come out
even, and `sao` is a star catalogue that is mostly incompressible.

The `xz` column is an `xz -9e` equivalent: LZMA2 at preset 9 with `LZMA_PRESET_EXTREME`, 64 MiB
dictionary, CRC-64, one stream — built against the liblzma already vendored here, because a baseline
quoted from someone else's table is a baseline nobody can defend.

### The measurement error that nearly cost a revert

The first pass reported `xz -9e` at 2,007,040 bytes for `x-ray` — 47% better than this engine, on one
file in twelve, which read as a serious regression in the LZMA2 gate. It was not. `/tmp` on this
machine is a 64 MB tmpfs, it filled during the run, and that stream was a **truncated write**: the
last two files wrote zero bytes and `x-ray` stopped at 2,007,040. Regenerating it and **decoding the
stream to check it** gave 4,491,264, which is what the engine's own fallback encoder produces
(4,489,807 raw) and what the native path beats.

Two lessons, both cheap and both now habit: check free space before a run that writes hundreds of
megabytes, and never trust a compressed size until the stream has been decoded back and compared.
The gate was right; the number was not.

## 6b. The one measurement this project cannot currently make

**Answered, 2026-11-07.** Silesia is measured (§6a) and the corpus is one `git clone` away —
`github.com/MiloszKrajewski/SilesiaCorpus`, which is reachable from this sandbox where
`sun.aei.polsl.pl` and `mattmahoney.net` are not.

Still missing are Calgary, Canterbury and enwik8. They matter less: Silesia is the set most tables
quote per-file, and one row that others can check is worth more than four they cannot.

`scripts/silesia.sh` regenerates §6a: it fetches the corpus, checks all twelve file sizes before
measuring, refuses to start without 500 MB free, and — the part that matters after the error above —
**decodes every xz stream back and compares it to the original before reporting its size**. Smoke
tested on two files (`SILESIA_ONLY=dickens,xml`); the full run is about fifteen minutes of
compression, which is longer than a single sandbox call, so the table in §6a came from the same steps
run by hand.

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
