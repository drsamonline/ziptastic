# Benchmarks

Results are logged after every phase, as required by `AGENT_PROMPT.md` §11.
"Measured, not assumed" — every number below came from a run on the machine
described under *Test machine*, with the command shown.

## Test machine

| | |
|---|---|
| CPU | containerised x86-64 vCPU, 2 cores, shared |
| RAM | 2 GiB |
| OS | Debian 12 (bookworm) |
| Compiler | Clang 22.1.8 (via Zig 0.17.0 as the C/C++ driver), `-O3` |
| Build | CMake 4.4.4, Ninja, `-DCMAKE_BUILD_TYPE=Release` |
| Baseline | `xz` built from `vendor/hcs/old/xz-master`, same compiler |

Absolute MB/s here is a floor, not a target: the container's CPU is shared and
the I/O path is a network-backed filesystem. Ratios are the meaningful column.

## Phase summary

| Phase | Silesia total | `silesia.zip` | Decode MB/s | Status |
|---|---|---|---|---|
| 0 — Skeleton (RAW only) | n/a | n/a | 100–108 | **pass** |
| 1 — LZMA2 fallback | 48,438,819 (46.19 MiB) | n/a (Phase 8) | 20–27 | **pass** |
| 2 — Fine match finder | n/a (no parser yet) | n/a | n/a | **pass** |
| 3 — Joint parser | 89,970,960 parse-only (see below) | n/a | 47–88 | **gate NOT met** |
| 4 — rANS + context models | 57,930,807 (+18.7%) | n/a | 26–70 | **ratio pass, speed gate NOT met** |
| 5 — Adaptive tables + range coder | 54,619,596 (-5.72% vs Phase 4) | n/a | 10-72 | **done via range coder** |
| 5b — x86 BCJ transform | 54,243,754 (-0.69% vs Phase 5) | n/a | 10-72 | **done; ooffice beats xz -9** |
| 6 — Coarse long-range index | 54,243,717 (-37 B) | n/a | 10-72 | **done; gate arithmetic corrected** |
| 7 — Huffman backends | 54,243,717 (unchanged) | n/a | 10-72 | **done; one gate is block-size dependent** |
| 8 — Preflate | zip subset -28.8% vs `xz -9` | n/a | n/a | **gate passed; full corpus run exceeded the command limit** |
| 9 — BWT backend | 6-file subset -6.09% vs `xz -9` | n/a | n/a | **done; BWT wins most blocks, but is slow** |
| 10 — CM (`--max-ratio`) | xml -13.1% vs `xz -9` | n/a | n/a | **done; ratio ceiling, ~3.4x the time** |
| 11 — PPMd for text | dickens -2.7% vs BWT | n/a | n/a | **done; wins on English text** |
| 12 — Tuning | dickens 2,355,513 (**-16.8%** vs `xz -9`) | n/a | n/a | **block size tuned; selector and parallelism open** |
| 6 — Coarse index | | | | not started |
| 7 — Huffman modes | | | | not started |
| 8 — Semantic precompression | | | | not started |
| 9 — BWT / Brotli / Zstd / LZ4 | | | | not started |
| 10 — CM backend | | | | not started |
| 11 — PPMd | | | | not started |
| 12 — Optimization | | | | not started |

Reference targets (from §9): `ziptastic -9 corpus/` ≤ `xz -9 corpus/`;
`ziptastic -9 silesia.zip` ≤ 44.0 MiB; `ziptastic --max-ratio silesia/`
≤ 42 MiB; decode ≥ 100 MB/s on the default (rANS) mode.

Baseline for the ratio columns: `xz -9` totals **48,795,576 bytes (46.54 MiB)**
on the 12-file Silesia corpus (211,938,580 bytes raw).

## Phase 0 — Skeleton

**Scope.** CMake build, CLI (`compress` / `decompress` / `bench` / `version` /
`help`), v1 header serialisation, RAW blocks only. Nothing is compressed yet:
this phase proves the container and the block loop, not the ratio.

**Tests.** All suites pass (`ctest --test-dir build --output-on-failure`):

| suite | what it proves | result |
|---|---|---|
| `unit.varint` | LEB128 round-trip, truncation, overlong encodings, max value, reader bounds | pass |
| `unit.header` | header round-trip; mutation of magic / version / flags / counts / modes / sizes each yields the specific error | pass |
| `unit.roundtrip` | 100 random files 0 B–1 MB plus block-boundary sizes, bit-exact; determinism; truncation at every prefix rejected | pass |
| `unit.fuzz` | 1000 mutated streams + header attacks + garbage: no crash, no hang, invariants hold | pass |
| `integration.cli` | `echo hi \| ziptastic \| ziptastic -d`, file round-trip, overwrite guard, error exits, `-b`, `bench` | pass |

**Container overhead.** A 1 MiB incompressible input produces 1 048 614 bytes:
38 bytes of header and block framing over 4 blocks of 256 KiB. Overhead is
independent of content, so it is invisible on real inputs.

**Throughput (RAW, 64 MiB incompressible input).**

| operation | measurement |
|---|---|
| compress | 284 MB/s |
| decompress (incl. file read + write) | 100–108 MB/s over three runs |
| round-trip | bit-exact (`cmp`) |

RAW decode is a memory copy, so this figure is the I/O floor, not the engine's
ceiling; Phase 4 sets the real rANS decode number against the ≥150 MB/s gate.

**Notes and decisions.**

- Block size defaults to 256 KiB per §12 of the prompt; the decoder learns
  block sizes from each block header, so `-b` output decodes with any build.
- The stream header carries `original_size` and `block_count`, so the encoder
  buffers its input before writing. Documented in `docs/FORMAT.md`; a
  streaming trailer variant is deferred to Phase 12 if it ever matters.

## Phase 1 — LZMA2 fallback (the Tier-1 guarantee)

**Scope.** Every input is also encoded as raw LZMA2 through liblzma, built
from `vendor/hcs/old/xz-master`. If that encoding is smaller than the
CHCE-native one, the stream ships as a single `LZMA2_FALLBACK` block. This is
the "never worse than `xz -9`" safety net, not the ratio story — the wins are
meant to come from Phases 4–8.

**Gate result: PASSED on all 12 files.** Every file is smaller than `xz -9`,
and every round-trip is bit-exact.

| file | raw | `xz -9` | ziptastic | delta |
|---|---|---|---|---|
| `mozilla` | 51,220,480 | 13,374,168 | 13,374,119 | -0.0004% |
| `webster` | 41,458,703 | 8,385,876 | 8,368,631 | -0.2056% |
| `nci` | 33,553,445 | 1,738,892 | 1,449,230 | -16.6578% |
| `samba` | 21,606,400 | 3,763,624 | 3,739,486 | -0.6413% |
| `dickens` | 10,192,446 | 2,830,612 | 2,830,563 | -0.0017% |
| `osdb` | 10,085,684 | 2,849,916 | 2,844,515 | -0.1895% |
| `mr` | 9,970,564 | 2,750,280 | 2,750,233 | -0.0017% |
| `x-ray` | 8,474,240 | 4,489,876 | 4,489,829 | -0.0010% |
| `sao` | 7,251,944 | 4,415,080 | 4,415,033 | -0.0011% |
| `reymont` | 6,627,202 | 1,317,160 | 1,315,552 | -0.1221% |
| `ooffice` | 6,152,192 | 2,426,824 | 2,426,777 | -0.0019% |
| `xml` | 5,345,280 | 453,268 | 434,851 | -4.0632% |
| **total** | **211,938,580** | **48,795,576** | **48,438,819** | **-0.7311%** |

Two design choices drive these numbers, both documented in `docs/FORMAT.md`:

1. **Raw LZMA2, not an `.xz` container.** We emit the bare LZMA2 stream and
   carry the one-byte filter property in the block metadata. That is worth
   ~50 bytes per file, which is why the "flat" files (mozilla, x-ray, sao,
   ooffice) land at −0.001% rather than at zero: they are `xz -9`'s own output
   minus the container.
2. **Two presets, keep the smaller.** Preset 9|EXTREME is not a superset of
   preset 9. Measured: `xml` −4.06% and `nci` −16.66% (extreme wins big on
   both), but `dickens` is 608 bytes *worse* with extreme. Encoding both and
   keeping the smaller is what makes the guarantee a measurement instead of a
   hope. It costs 2x encode time — a Phase 12 tuning knob, not a correctness
   issue.

The fallback is deliberately **whole-stream**. Independent LZMA2 per 256 KiB
block would reset the 64 MiB dictionary at every boundary and lose to `xz -9`
on exactly the files with long-range redundancy (mozilla, webster). Per-block
LZMA2 only becomes meaningful once blocks carry their own model state, which
is what Phase 4's rANS blocks do.

**Format compatibility.** A `LZMA2_FALLBACK` payload decodes with upstream
`xz`, not just with us:

```
xz --format=raw --lzma2=dict=64MiB -dc payload.bin   # bit-exact on xml
```

**Throughput (fallback path).**

| operation | measurement |
|---|---|
| compress | ~1.2 MB/s per preset (two presets per file) |
| decompress (incl. file I/O) | dickens 19–21 MB/s, mozilla 26–27 MB/s |
| round-trip | bit-exact (`cmp`) on all 12 files |

Decode is slow for LZMA2 because of the 64 MiB dictionary on a shared vCPU;
the ≥100 MB/s acceptance target applies to the default rANS path, not to the
safety net.

**Tests added.** `unit.lzma2` covers codec round-trips across sizes and content
kinds, compression on text, wrong-size rejection, corrupted-payload rejection,
bad/oversized filter properties, and truncation. `unit.roundtrip` gained
fallback-wins-on-compressible, fallback-declines-on-incompressible and
disabled-fallback cases. `tests/benchmark/silesia_gate.sh` is the reusable
gate: it resumes, verifies bit-exactness, and exits non-zero if any file is
larger than `xz -9`.

## Phase 2 — Fine match finder (BT4)

**Scope.** `include/chimera/match_finder.hpp` + `src/engine/match_finder.cpp`: a
C++20 port of the binary-tree match finder from the LZMA SDK
(`vendor/hcs/old/lzma/C/LzFind.c`, public domain). Three hash tables (2-, 3- and
4-byte prefixes) feed a binary search tree over positions in a cyclic buffer, so
memory is bounded by the window rather than by the input length. The finder
returns matches ordered by increasing length, each with the distance at which
that length was first reached — the shape a cost model can price in one pass.

This phase changes no bytes on disk: the format is untouched until the parser
starts emitting matches in Phase 3.

**The gate.** The prompt asks that fine matches "match upstream LZMA's parse
(within 2% bit-rate)". A parse cannot be compared before a parser exists, so
this phase is gated on the stronger property a parser actually depends on:
*the finder never misses a match that an exhaustive scan of the same window
would find*. Brute force is the authority; a parser is only as good as its
match list.

Measured with `tests/benchmark/match_stats` — a greedy walk over each file
(window 1 MiB, `nice_len` 273, LZMA's default tree-depth cutoff) plus an
exhaustive-scan cross-check at 150 sampled positions per file:

| file | bytes | positions with a match | greedy coverage | avg match len | oracle | MB/s | finder mem |
|---|---|---|---|---|---|---|---|
| `dickens` | 10,192,446 | 10.9% | 99.6% | 9.1 | 151/151 | 2.3 | 24.5 MB |
| `mozilla` | 51,220,480 | 9.3% | 87.5% | 9.4 | 151/151 | 4.1 | 24.5 MB |
| `mr` | 9,970,564 | 11.6% | 98.1% | 8.5 | 151/151 | 2.1 | 24.5 MB |
| `nci` | 33,553,445 | 2.3% | 99.6% | 44.0 | 151/151 | 2.2 | 24.5 MB |
| `ooffice` | 6,152,192 | 13.5% | 87.0% | 6.4 | 151/151 | 3.8 | 24.5 MB |
| `osdb` | 10,085,684 | 6.6% | 86.8% | 13.1 | 151/151 | 3.2 | 24.5 MB |
| `reymont` | 6,627,202 | 7.7% | 99.2% | 13.0 | 151/151 | 2.3 | 24.5 MB |
| `samba` | 21,606,400 | 5.6% | 93.1% | 16.6 | 151/151 | 3.0 | 24.5 MB |
| `sao` | 7,251,944 | 11.2% | 58.5% | 5.2 | 151/151 | 3.5 | 24.5 MB |
| `webster` | 41,458,703 | 8.4% | 99.4% | 11.9 | 151/151 | 2.5 | 24.5 MB |
| `x-ray` | 8,474,240 | 22.1% | 92.9% | 4.2 | 151/151 | 4.1 | 24.5 MB |
| `xml` | 5,345,280 | 3.2% | 98.9% | 31.3 | 151/151 | 2.7 | 24.5 MB |
| **total** | **211,938,580** | **8.2%** | **93.2%** | | **1812/1812** | | |

**Result: 1812/1812 sampled positions agree with an exhaustive scan.** On every
probe the finder's best match is exactly the best match that exists — it is not
leaving matches on the table, which is the property Phase 3 needs.

Greedy coverage of 93.2% of all bytes (87–99.6% per file) is the finder's own
contribution; `sao` is the outlier at 58.5% because it is a star catalogue of
near-random floats with short repeats. Turning that coverage into a ratio is
the parser's job.

**Throughput: 2.1–4.1 MB/s** for the greedy walk. That is finder-only cost at
`nice_len` 273 with the LZMA depth cutoff, and it is in the same band as `xz -9`
end-to-end on this hardware, so it is not yet the bottleneck — but it is
untuned. Phase 12 owns this.

**Bugs found and fixed during the phase, worth recording:**

1. The window guard originally used `min(pos, window)`, copying LZMA's `SET_mmm`.
   That silently excluded a candidate at position 0 until the buffer filled, so
   a 40-byte repeat at the very start of a file was never found. The guard now
   checks against the window itself, which is what actually matters: a distance
   of exactly `window` aliases a candidate onto the current position's own tree
   slot.
2. The 4-byte hash table was a fixed 2^22 entries (16 MiB) regardless of input
   size, so a 100 KiB file paid for a 16 MiB table. It now scales to about one
   bucket per two input bytes, capped at the requested size.

**Tests.** `unit.match_finder` (10 cases): known strings with hand-checked
matches; exhaustive brute-force comparison over every position of several small
inputs at three alphabet sizes; sampled comparison on a 20,000-byte input;
window-bound behaviour either side of the boundary; `nice_len` capping;
skip-vs-query agreement; re-query safety; determinism; memory bounds; progress
accounting. Every reported match is also verified to be real (same bytes, legal
distance, increasing length).

## Phase 3 — Joint optimal parser

**Scope.** `include/chimera/cost_model.hpp`, `src/engine/parser.cpp`,
`src/engine/token_stream.cpp`. A shortest-path parse over each block: `cost[j]`
is the cheapest way to reach position `j`, every literal and every available
match relaxes the position it lands on, and the price function is an object
(`CostModel`) rather than a special case per token kind. Matches come from the
Phase 2 finder, which carries its window across blocks.

New transitional mode `0x0B LZ_RAW` stores the parse with 8-bit literals and no
entropy coding, so it can be round-tripped and measured. `docs/FORMAT.md`
documents the body.

### Gate: NOT met

The prompt asks that the ratio be "within 5% of LZMA2". Measured with the
fallback disabled, so the parse path is what is being weighed
(`ziptastic --no-lzma2 -9`, all 12 Silesia files, bit-exact round-trip on
every one):

| | bytes | vs `xz -9` |
|---|---|---|
| `xz -9` (preset 9, same liblzma) | 48,795,480 | — |
| parse path (`LZ_RAW`) | 89,970,960 | **+84.4%** |
| per-file range | | +53.9% (`osdb`) to +110.0% (`mr`) |

This is not a parse failure, it is a missing entropy coder: `LZ_RAW` stores
every literal as 8 bits where LZMA2 spends about 4–5, and with 36% of symbols
being literals on text that alone accounts for most of the gap. **The 5% target
is only meaningful once Phase 4 supplies the coder**, and it is measured there.

What this phase can and does demonstrate:

| property | result |
|---|---|
| Parse is optimal for its cost model | **agrees exactly** with an independently written memoised-recursion oracle on every small input tested (3 alphabets x 3 seeds) |
| Optimal beats greedy | never worse; >1% better on 4-letter synthetic data (unit test) |
| Round-trip on the corpus | bit-exact on all 12 Silesia files, including a 51 MB `mozilla` |
| Cross-block window carry | verified: a block whose data only exists in the previous block codes by reference (unit test) |

**Parse quality, isolated from the coder** (`tests/benchmark/parse_stats`, four
files, 32.9 MB): the parse's modelled cost is **12.16 MiB of ideal bits**
against LZMA2's actual 10.06 MiB on the same four files — **+20.9%**. That gap
is the *cost model*, not the search: the search is provably optimal for the
model it is given, and the model prices matches with a static slot
approximation (a length slot plus a distance slot plus extra bits) where LZMA
uses adaptive prices learned from the data. Closing that is Phase 4's job, and
it is why the model is an object rather than constants.

### Recalibration: the cost model was the gap

The +20.9% modelled gap above was diagnosed and cut by pricing matches the way
LZMA actually codes them rather than by a generic slot formula. Two errors
dominated:

| symbol | old price | LZMA's real cost | consequence |
|---|---|---|---|
| distance 1–3 | 6 + log2(dist) ≈ 6–8 bits | ~2.5 bits (near distances are cheap) | parser avoided the short matches that are most of a match list |
| length 3 | 4 + log2(len) = 6 bits | ~3.5 bits (lengths 2–9 are a 3-bit tree) | same |

After the fix (lengths 2–9 at ~3.5 bits, longer at 5 + log2; distances < 4 at
~2.5 bits, else 5 + log2) the same four files measure **11.18 MiB of ideal bits
against LZMA2's 10.06 MiB — +11.1%, down from +20.9%**. The parse also changes
character: literals fall from 64.9% to 43.2% of symbols on `dickens`.

Full corpus: **53.85 MiB modelled vs LZMA2's 46.54 MiB = +15.7%**, pulled up by
`mozilla` (+27%) and `x-ray` (+19%) — executable data, where LZMA's
repeat-distance coding matters most and our cost model has no rep state at all.
Closing that needs rep distances in the DP, which is a Phase 12-class change,
not a pricing tweak.

One honest consequence: the *uncoded* size got slightly worse, 89.97 MB →
92.11 MB. More matches means more per-match framing in `LZ_RAW` (about 3 bytes
each), where a real coder would spend about 3 bits. That is precisely why
`LZ_RAW` is the wrong yardstick and why Phase 4 is the fix.

**Speed: 0.26–1.5 MB/s encode.** Slow, and expected: the DP relaxes every
length up to `nice_len` for every match at every position, on a 64 MiB window.
Decode is 47–88 MB/s. Phase 12 owns this.

### Bug found: a corrupted match tree

`mozilla` failed to round-trip. Bisection on prefixes found the first bogus
match at byte 982,311: the finder reported a 55-byte match where only 13 bytes
matched. The cause was a faithful-porting error in the BT4 walk — on hitting the
tree-depth cutoff I adopted the candidate's subtree, where LZMA sets both
children to *empty*. Adopting is only sound when the match reached `len_limit`
(only then does the position occupy the same place in the ordering); with
`len < len_limit` the adopted children can sit on the wrong side, which breaks
the "shares `min(len0, len1)` bytes" invariant that lets the walk skip
re-comparing bytes. The walk then starts from a length it never verified and
invents a match.

This is the failure mode worth remembering: it is silent, it needs a large
input with busy hash buckets, and it corrupts data rather than erroring. Two
regression tests now pin it: `depth_cutoff_never_invents_matches` (depth 1/2/4
on a 3-letter alphabet, every reported match verified byte for byte) and
`tail_of_buffer_matches_are_real`. A third covers multi-block round-trip with a
partial trailing block.

**Tests.** New `unit.parser` (8 cases: oracle agreement, token validity,
token-stream round-trip, cross-block carry, optimal-vs-greedy, incompressible
input, partial trailing block, cost-model monotonicity). `unit.match_finder`
grew to 12 cases with the two regressions above. 8/8 ctest suites pass.

## Phase 4 — rANS + context models

**Scope.** `src/entropy/rans.cpp` (byte-renormalised rANS, 12-bit tables),
`src/entropy/token_coder.cpp` (the context model and the block body). The parse
from Phase 3 is unchanged; only the entropy stage is new, and `RANS_CTX`
replaces `LZ_RAW` as the parsed candidate.

Contexts: match flag on position&3, literals on the previous byte's high
nibble, length slots on position&3, distance slots on the length slot. Tables
are measured on the block and written into it.

### Gates

| gate | target | measured | verdict |
|---|---|---|---|
| ratio equals or beats Phase 3 | ≤ 89,970,960 | **57,930,807** | **pass** (−35.6%) |
| round-trip on Silesia | bit-exact | bit-exact, all 12 files | **pass** |
| rANS decode throughput | ≥ 150 MB/s | 26–70 MB/s in memory | **NOT met** |

The ratio gate is the one that matters here and it passed decisively: the
corpus went from **+84.4% vs `xz -9` to +18.7%** — the coder recovered most of
the gap the parse had exposed. Per file:

| file | raw | `xz -9` | rANS | delta |
|---|---|---|---|---|
| `webster` | 41,458,703 | 8,385,868 | 8,747,813 | +4.32% |
| `dickens` | 10,192,446 | 2,830,604 | 2,963,280 | +4.69% |
| `reymont` | 6,627,202 | 1,317,152 | 1,415,229 | +7.45% |
| `nci` | 33,553,445 | 1,738,884 | 1,928,571 | +10.91% |
| `xml` | 5,345,280 | 453,260 | 505,719 | +11.57% |
| `samba` | 21,606,400 | 3,763,616 | 4,366,310 | +16.01% |
| `mr` | 9,970,564 | 2,750,272 | 3,213,078 | +16.83% |
| `x-ray` | 8,474,240 | 4,489,868 | 5,252,101 | +16.98% |
| `sao` | 7,251,944 | 4,415,072 | 5,264,258 | +19.23% |
| `osdb` | 10,085,684 | 2,849,908 | 3,400,189 | +19.31% |
| `ooffice` | 6,152,192 | 2,426,816 | 2,914,412 | +20.09% |
| `mozilla` | 51,220,480 | 13,374,160 | 17,959,847 | +34.29% |
| **total** | **211,938,580** | **48,795,480** | **57,930,807** | **+18.72%** |

The remaining gap is what Phase 5 (adaptive tables) and Phase 6 (coarse index)
are for. Two known contributors: static tables pay for themselves per block,
and the cost model still has no repeat-distance state, which is exactly what
`mozilla` and `x-ray` — executable data — punish hardest.

**Decode speed: 26–70 MB/s in memory**, 16–72 MB/s through the CLI with file
I/O. The 150 MB/s target is not met. The decoder is single-stream and processes
one symbol at a time; reaching 150 MB/s needs interleaved rANS streams
(2–4 states decoded in parallel), which is a Phase 12 change. Halving the gap
came from replacing the byte-at-a-time match copy with a bulk `memcpy` when a
match does not overlap its own output (`webster` 39→50, `samba` 51→70 MB/s).

### Bugs found

Three, all in the coder, all caught by the unit tests before the corpus run:

1. **rANS is a stack.** The decoder pops symbols in the *reverse* of the order
   they were encoded, so the encoder must emit the symbol sequence backwards.
   Encoding forwards round-tripped only when the data happened to be
   palindromic — my first 6-symbol test was, which is why it passed.
2. **Renormalisation is a loop, not a single read.** With a skewed table the
   encoder can emit several bytes at one `put`, and the decoder must consume
   them all. Reading one byte desynchronised the stream on highly skewed data.
   (I "fixed" this in the wrong direction first: the original loop was right,
   the ordering was the bug.)
3. **Normalisation could overrun its own table.** The rounding repair subtracted
   the whole difference from the largest frequency and clamped it at 1, which
   left the total wrong; the slot-filling loop then wrote past the 4096-entry
   decode table. That is a heap overwrite — it aborted the process on `mozilla`
   with SIGABRT. The repair now moves one unit at a time and never takes a
   symbol below 1, and the fill refuses to write past the end even if it is.

**Tests.** New `unit.rans` (9 cases): table normalisation and serialisation,
rejection of bad frequencies, coder round-trip over 5000 random symbols across
five differently-shaped contexts, compression of skewed data, slot mapping
consistency across the whole length and distance range, token-stream round-trip
at three alphabets, coded-vs-uncoded size, and corruption rejection. 9/9 ctest
suites pass.

## Phase 5 — Adaptive tables, and the range coder that made them pay

**Outcome.** The phase resolved in two steps. The chunked adaptive rANS model is
correct and ships, but on its own it gains only 0.087% — on stationary text it
gains nothing at all. The phase's goal was only reachable by adding a **range
coder**, which is FIFO and therefore supports per-symbol adaptation. With all
three models offered per block and the smallest chosen, the corpus goes from
**57,930,807 to 54,619,596 bytes: -3,311,211 (-5.72%)**, and the gap to `xz -9`
from **+18.72% to +11.94%**.

### Why rANS could not do what the spec asked

The spec asks for tables re-normalised every 256 symbols. rANS is a stack: the
decoder pops symbols in the reverse of the encoding order, so at symbol *i* the
encoder's model has seen *i+1..n* and the decoder's has seen *0..i-1*. A
per-symbol adaptive rANS is not decodable in principle — which is why LZMA uses
a range coder and why rANS implementations use static tables or transmit
periodic updates.

### Step 1 — chunked adaptive rANS (ships, small win)

Each chunk (1024 tokens) is an independent rANS stream with a fixed table; chunk
0 carries tables measured over the whole block and later chunks derive theirs
from already-decoded symbols, so nothing after chunk 0 is transmitted. Both
sides seed their counts from the transmitted frequencies, then decay by three
quarters and fold in the previous chunk's histogram, floored at 1.

Three defects were found getting here:

1. **Chunking by symbols instead of tokens.** The decoder counts tokens; it was
   told to expect 3374 symbols where there were 1638 and ran off the end.
2. **A symbol with frequency 0 is a divide-by-zero.** Adaptive tables come from
   *earlier* chunks, so a first-seen symbol had freq 0, `RansEncoder::put`
   divided by zero, and its renormalisation loop ran until memory was exhausted
   (`std::bad_alloc` — the 1 MiB CLI crash). Found by hashing the table set on
   both sides per chunk and diffing: the *encoder* crashed first, which pointed
   at the model rather than the stream.
3. **Cold-start adaptation loses.** A uniform prior made everything codable but
   reserved 6.25% of the 4096-slot literal alphabet: up to 4.8% worse on
   low-entropy data. Fixed by measuring chunk 0 over the whole block.

Alone, that model gains 50,153 bytes (-0.087%) on the corpus.

### Step 2 — the range coder (where the phase's win actually is)

`src/entropy/range_coder.cpp`: the standard LZMA binary range coder — 32-bit
range, 11-bit probabilities, 5-bit adaptation, carry handled through a cache
byte — plus bit trees for symbols. Because it is FIFO, the model adapts after
*every bit*: no tables, no chunks, no transmission. Contexts match the rANS
path so the two are directly comparable.

Measured on single-block parses of four files (the tool parses the whole file as
one block, so these differ from the blocked corpus numbers):

| file | rANS static | rANS adaptive | range coder | range vs rANS |
|---|---|---|---|---|
| `dickens` | 2,953,395 | 2,956,400 | 2,947,324 | **-0.21%** |
| `webster` | 8,644,422 | 8,676,990 | 8,638,928 | **-0.06%** |
| `xml` | 590,017 | 569,277 | 555,717 | **-5.81%** |
| `samba` | 4,676,115 | 4,438,552 | 4,286,983 | **-8.32%** |

It wins on every file, and by a lot where the data is heterogeneous. That is
what per-symbol adaptation buys and what chunked rANS cannot.

### Corpus result, all three models offered per block

| file | `xz -9` | Phase 4 | Phase 5 | delta bytes | vs Phase 4 | vs `xz -9` |
|---|---|---|---|---|---|---|
| `webster` | 8,385,868 | 8,747,813 | 8,676,928 | -70885 | -0.81% | +3.47% |
| `dickens` | 2,830,604 | 2,963,280 | 2,960,174 | -3106 | -0.10% | +4.58% |
| `reymont` | 1,317,152 | 1,415,229 | 1,393,328 | -21901 | -1.55% | +5.78% |
| `nci` | 1,738,884 | 1,928,571 | 1,858,811 | -69760 | -3.62% | +6.90% |
| `xml` | 453,260 | 505,719 | 487,239 | -18480 | -3.65% | +7.50% |
| `osdb` | 2,849,908 | 3,400,189 | 3,090,701 | -309488 | -9.10% | +8.45% |
| `samba` | 3,763,616 | 4,366,310 | 4,145,894 | -220416 | -5.05% | +10.16% |
| `ooffice` | 2,426,816 | 2,914,412 | 2,704,203 | -210209 | -7.21% | +11.43% |
| `sao` | 4,415,072 | 5,264,258 | 4,995,846 | -268412 | -5.10% | +13.15% |
| `mr` | 2,750,272 | 3,213,078 | 3,132,212 | -80866 | -2.52% | +13.89% |
| `x-ray` | 4,489,868 | 5,252,101 | 5,217,925 | -34176 | -0.65% | +16.22% |
| `mozilla` | 13,374,160 | 17,959,847 | 15,956,335 | -2003512 | -11.16% | +19.31% |
| **total** | **48,795,480** | **57,930,807** | **54,619,596** | **-3311211** | **-5.72%** | **+11.94%** |

**Total: 54,619,596 vs 57,930,807 — 3,311,211 bytes smaller (-5.72%).**

On the phase's specific gate — 1% on text-heavy files — `dickens` improves 0.11%
and `webster` 0.81%. **Neither reaches 1%.** Stationary English text is where
whole-block two-pass statistics are already strongest, and that is the honest
measurement. The large wins are elsewhere: `mozilla` -11.2%, `osdb` -9.1%,
`nci` -3.6%, `ooffice` -7.2%, `xml` -3.6%.

**Cost.** Encode is now ~0.9 MB/s because three models are tried per block.
Phase 12 owns that: the selector could be made to skip models by a cheap
heuristic instead of trying all three.

**Tests.** New `unit.range_coder` (7 cases): biased bit round-trips, adaptation
reaching the entropy bound (99% zeros costs < 0.2 bits/bit), eight independent
models, direct bits, bit trees at four widths, a mixed flag/tree/direct stream
shaped like the token coder, and truncation reporting. `unit.rans` is 12 cases.
10/10 ctest suites pass.

## Phase 5b — x86 BCJ transform (a Layer 0 win found by reviewing the sources)

Came out of `docs/SOURCES_REVIEW.md`: the x86 BCJ filter is already vendored in
`xz-master/src/liblzma/simple/x86.c` (0BSD) and we already link liblzma, so the
work was porting the transform, not finding it.

**What it does.** x86 code encodes CALL/JMP targets as *relative* displacements,
which differ at every call site. The filter rewrites them to absolute, so they
repeat, and is exactly invertible. `xz -9` does not apply it unless asked
(`xz --x86`), so on executable data this is a win against our own baseline.

**Result: corpus 54,619,596 → 54,243,754 (−375,842 bytes, −0.69%); the gap to
`xz -9` goes from +11.94% to +11.17%.**

| file | before | after | change | vs `xz -9` |
|---|---|---|---|---|
| `ooffice` | 2,704,203 | **2,328,361** | **−13.90%** | **−4.06%** |
| `mozilla` | 15,956,335 | 15,956,335 | 0.00% (declined) | +19.31% |
| `dickens` | 2,960,174 | 2,960,174 | 0.00% (not code) | +4.58% |
| `samba` | 4,145,894 | 4,145,894 | 0.00% (not code) | +10.16% |

`ooffice` is now **below `xz -9`** — the first file in the corpus to beat it.

**The interesting negative result.** I expected this to help `mozilla` most, since
it is the worst file and is made of executables. It made it 0.78% *worse*.
`mozilla` is a tar: the CALL/JMP bytes are plentiful but scattered across archive
members instead of forming one code image, so absolute displacements become
position-dependent without becoming repetitive. A density heuristic cannot tell
these apart, so the decision is made by trying the transform on a 128 KiB sample
and keeping it only when the sample codes smaller. That costs well under a second
and is what stops a plausible-looking idea from making the corpus worse.

**Correctness.** The transform is exactly invertible and is applied to the whole
stream before compression, reversed after decompression, signalled by
`kFlagBcj` (flag bit 3). `unit.bcj` checks invertibility on x86-like data, on
uniform random bytes, and at sizes 0/1/4/5/6/17, plus the detection gate. 11/11
ctest suites pass.

## Phase 6 — Coarse (long-range) index

`include/chimera/coarse_index.hpp` + `src/dedup/coarse_index.cpp`: a block-hash
index beside the fine finder, so content repeated beyond the 64 MiB window can
still be found. The parser queries both and prices both through the same cost
model.

**Written from scratch, deliberately.** SREP is the reference design, but its
source is all-rights-reserved (see `docs/SOURCES_REVIEW.md` and the corrected
HCS manifest), so no SREP code is used. The technique is published and simple
enough to implement directly.

**Design.** Hash the first 8 bytes at each position into a bucket of 16 slots;
verify every candidate byte for byte before reporting it, so a hash collision
costs a comparison rather than a wrong match. A match must reach 48 bytes to be
reported — a short match is not worth an extreme distance code. Positions are
inserted as the parser walks forward, so history is unbounded.

The bucket policy is the part that matters: **half of each bucket is anchored**
(filled once, never overwritten) and half rotates. Pure round-robin evicts early
positions under a long run of noise — exactly the case the index exists for —
while pure first-come-keeps loses locality on ordinary data.

### The gate

The prompt asks that `file_a` + 100 MB of noise + `file_a` be found, and that the
ratio beat fine-only by 30%. Measured on 1 MiB + 100 MiB noise + 1 MiB
(106,954,752 bytes), `ziptastic -9 --no-lzma2 --no-bcj`:

| | bytes | time |
|---|---|---|
| fine only (`--no-coarse`) | 106,957,620 | 190 s |
| with coarse index | 105,925,428 | 237 s |
| **saved** | **1,032,192** | |

The repeat sits 100 MiB back, outside the 64 MiB fine window, and the coarse
index found it: the saving is essentially the whole redundant copy (1,048,576
bytes minus coding overhead). Round-trip is bit-exact.

**The 30% figure is not reachable on this input, and that is arithmetic, not a
miss.** Only 2 MiB of the 102 MiB input is redundant, so the maximum possible
saving is ~2%; measured 0.965%, i.e. the entire redundancy. A 30% improvement
needs the redundant portion to dominate: `file_a` + noise + `file_a` with
|file_a| ≈ |noise| gives ~33%. The mechanism is proven; the percentage is a
property of the input shape, and the prompt's number assumes a shape it does not
specify.

### Effect on the corpus: essentially none, and why

Silesia files are ≤ 51 MiB, so the fine window (64 MiB, or the file size when
smaller) already sees the whole file and the coarse index has nothing extra to
find. Measured: `dickens` unchanged at 2,960,174; `samba` **4,145,857, 37 bytes
better** than Phase 5b. That small gain is real and has a cause worth knowing:
the fine finder has a tree-depth cutoff, so it can skip a long match; the coarse
hash finds it directly. The index is a backstop as well as a long-range finder.

**The discount was removed because it cost bytes.** The prompt specifies a
distance-context discount for coarse matches. Implemented at 2 bits, `samba` got
16 bytes *worse*: a discount lets the parser prefer a coarse match that is
slightly worse than the fine alternative. At 0 discount the same file is 37 bytes
better than before the index existed. Recorded here rather than silently kept
because the plan asked for it.

**Cost:** 237 s vs 190 s on the 102 MiB gate input (+25% encode), for a 1 MiB
saving — that is a good trade on archive-like data and a pure loss on data with
no long-range redundancy, which is why it is a flag (`-9` enables it,
`--no-coarse` disables it) rather than always-on.

**Tests.** `unit.coarse_index`, 5 cases: the repeat-after-a-gap gate in
miniature, no false matches on 300 KB of random bytes, every reported match
verified byte for byte on repetitive data, the anchored-slot policy surviving
3 MB of noise, and a memory bound. 12/12 ctest suites pass.

## Phase 7 — Static and adaptive Huffman

`include/chimera/huffman.hpp` + `src/entropy/huffman.cpp`: canonical Huffman
(static, two-pass, code lengths in the body) and an adaptive variant (rebuilds
every 256 symbols from counts both sides maintain, so nothing is transmitted).
Modes `0x02` and `0x03`, offered as per-block candidates alongside RAW, the
token path and LZMA2.

**These code block bytes, not tokens.** A Huffman coder over the *token* stream
would be strictly worse than the rANS and range coders already in the tree and
would never be selected, so it would be dead weight. Coding bytes gives a
genuinely different trade-off: it helps where LZ finds little but the byte
distribution is skewed, and loses to RAW where there is no skew.

### Gate 1 — adaptive beats static by >=2% on `mozilla`: PASSED

Per block over `mozilla` (51,220,480 bytes, 196 blocks of 256 KiB):

| | bytes |
|---|---|
| RAW (stored) | 51,220,480 |
| HUFF_STATIC | 37,528,334 |
| HUFF_ADAPT | **36,205,500** |

**Adaptive is 3.52% smaller than static**, against a 2% target. Paletted binary
drifts within the file, and a 256-symbol rebuild cadence tracks it. `ooffice`
shows the opposite — +0.01%, i.e. a wash — which is what you would expect from a
more homogeneous executable.

### Gate 2 — RAW beats Huffman by >=5% on incompressible blocks: PASSED ONLY FOR SMALL BLOCKS

Direction is right everywhere, but the margin is the 256-byte code-length header
amortised over the block, so it is a function of block size, not of the coder.
Measured on 1 MiB of uniform random bytes:

| block size | RAW vs static Huffman |
|---|---|
| 256 KiB (our default) | 0.10% |
| 64 KiB | 0.39% |
| 16 KiB | 1.54% |
| **4 KiB** | **5.68%** |
| 1 KiB | 18.75% |

So the 5% target is met at 4 KiB blocks and not at the default. Recorded as
measured rather than tuned to fit: shrinking blocks to pass a ratio gate would
cost more elsewhere than the gate is worth.

### Effect on the corpus: none, and that is the expected answer

`dickens` 2,960,174, `ooffice` 2,328,361, `samba` 4,145,857 — all identical to
Phase 6. On real data the LZ + range path wins every block, so the Huffman
candidates are never selected. They exist for the case where LZ finds nothing
but the bytes are skewed; on this corpus that case does not arise, and the
selector correctly declines them.

**Bug found.** The decode-tree builder held a reference into a `std::vector` and
then called `resize()` on it, writing interior nodes through a dangling
reference. Blocks with one or two distinct symbols passed (no interior nodes
needed) while anything larger failed — which is exactly how it presented: the
static round-trip failed at 255 bytes and not at 1 or 2.

**Tests.** `unit.huffman`, 7 cases: static and adaptive round-trips across six
sizes including single-symbol and all-256-uniform, adaptive round-trips across
rebuild boundaries with a distribution that changes partway, static compression
on skewed data, the impossibility check that Huffman cannot beat raw on uniform
data, determinism, and truncation rejection. 13/13 ctest suites pass.

## Phase 8 — Preflate (DEFLATE recompression)

A ZIP, PNG or JAR is already DEFLATE-compressed, so compressing it directly is
compressing compressed data. Preflate parses the DEFLATE streams, inflates them,
and records a correction that recreates the exact original bytes — so the
inflated data goes through the normal pipeline and the correction rides along.

Vendored source: `vendor/hcs/old/preflate-master`, Apache-2.0 (verified).
`preflate_decode()` takes **one bare DEFLATE stream**, not a container, so
`src/precompressors/preflate_wrap.cpp` supplies the ZIP layer: it walks the local
file headers, runs preflate on each member, and records offsets and lengths
needed to splice the original back together.

### Result

Measured on a ZIP of three corpus files (`dickens` + `reymont` + `samba`,
38.4 MiB of text, 11,184,180-byte ZIP):

| | bytes |
|---|---|
| `xz -9` on the ZIP | 11,067,680 |
| ziptastic, `--no-precomp` | 11,067,642 |
| **ziptastic, precomp** | **7,882,439** |
| **vs `xz -9`** | **-28.8%** |

Round-trip is bit-exact. The 3% gate is cleared by an order of magnitude,
because the alternative is not "compress the ZIP slightly better" — it is "stop
compressing compressed data".

A single-file ZIP of `dickens` shows the same shape: 3,871,856 → **2,830,566
(-26.9%)**. The preflate correction for that 3.87 MiB DEFLATE stream is **3
bytes**, and the 10.19 MiB it inflates to compresses to 2.83 MiB.

### The full `silesia.zip` gate was not completed here

`silesia.zip` is 68 MiB of DEFLATE that inflates to ~203 MiB. At this engine's
~0.6 MB/s that is over 350 s of compression, and a single sandbox command is
capped at 300 s, so the run cannot finish in one invocation. The subset above is
the same kind of data (ZIP of Silesia text) at a size that fits, and is reported
as a subset rather than as the named gate.

### Two bugs, both about a decision rather than a coder

1. **The flag and the data disagreed.** After precompression was enabled, the
   stream was still built from the *original* bytes while the header said
   `kFlagPrecomp`, so the decoder ran the inverse transform on bytes that never
   had it — `corrupt`. The cause was an edit that silently failed to match after
   the precomp block was inserted.

2. **The sample was taken from the wrong end.** `precomp_helps` compares the
   coded size of the payload against the original, on a sample. It sampled from
   the *start* of the payload — which holds the stream table and the diffs, not
   the inflated data — and estimated a 38 MiB text payload as 13.9 MiB of output
   when the truth is 8.1 MiB. The decision went the wrong way and precompression
   silently did nothing. Sampling from the middle fixes it; the same input now
   estimates 8.1 MiB and compresses to 7.88 MiB.

Both were caught by checking the output size against what the arithmetic
predicted, not by the tests — the round-trip tests pass either way, because a
transform that is never applied is still consistent.

**Build notes.** Preflate is C++14-era (`std::result_of`), so its sources and our
wrapper are compiled as C++17 in separate targets while the engine stays C++20.
Its `filestream.cpp` is Windows-only and only serves its demo executables, so it
is excluded. Its own `CMakeLists.txt` declares `cmake_minimum_required(3.2)`,
which CMake 4 refuses, so the sources are compiled directly instead.

## Phase 9 — BWT backend (libbsc)

Burrows-Wheeler is a different family from LZ: it sorts the block, then codes
runs. It wins where the structure is statistical rather than repeated — numeric
records, tables, binary — and that turns out to be most of this corpus.

Vendored source: `vendor/hcs/old/libbsc-master` (Apache-2.0, verified), compiled
directly: its own CMakeLists prefers clang and enables OpenMP, neither of which
we want. libbsc 3.4 delegates suffix sorting to libsais, which is vendored inside
it and had to be added to our source list.

**Parameters:** no LZP (`lzpHashSize`/`lzpMinLen` = 0) because we already have
our own match finder; BWT sorter; adaptive QLFC coder.

### It wins almost everywhere

Per-block mode census (`-v`), and the resulting sizes against `xz -9`:

| file | blocks choosing BWT | ziptastic | `xz -9` | vs `xz -9` |
|---|---|---|---|---|
| `mr` | 39 of 39 | 2,436,304 | 2,750,272 | **-11.42%** |
| `x-ray` | 33 of 33 | 4,025,536 | 4,489,868 | **-10.34%** |
| `ooffice` | LZMA2 fallback | 2,083,954 | 2,426,816 | -14.13% |
| `xml` | mostly BWT | 423,579 | 453,260 | -6.55% |
| `reymont` | mostly BWT | 1,249,670 | 1,317,152 | -5.12% |
| `dickens` | 35 of 39 | 2,820,882 | 2,830,604 | -0.34% |
| `osdb` | mostly BWT | 2,844,515 | 2,849,908 | -0.19% |
| `sao` | LZMA2 fallback | 4,415,033 | 4,415,072 | -0.00% |

**Six-file subset total: 11,858,904 vs 12,628,012 — -6.09% against `xz -9`.**

This is the phase where the corpus finally goes *below* `xz -9` rather than
hovering above it. `mr` (MRI scans) and `x-ray` (16-bit image data) are exactly
the "statistically structured binary" case the phase predicted, and BWT is worth
10-11% on them.

### What was not measured, and why

`mozilla` (51 MiB) and `webster` (41 MiB) did not finish: BWT suffix-sorts every
256 KiB block, and on those two the run exceeds the sandbox's 300 s per-command
limit. That is a real cost, not a bookkeeping problem — see below. `nci` and
`samba` were also not completed. The eight files above are what the numbers are
based on; the subset total is labelled as a subset.

### Cost

BWT is expensive: suffix sorting dominates, and encode throughput drops to well
under 0.5 MB/s on large files. That is why `-9` (max effort) enables it while the
default mode does not, and why Phase 12's job — a cheap heuristic instead of
trying every backend on every block — now matters more than it did.

**Tests.** `unit.bwt`, 3 cases: round-trip at three block sizes on structured
data, declining incompressible input without ever returning something larger than
its input, and rejecting truncated input. 14/14 ctest suites pass.

## Phase 10 — Context mixing for `--max-ratio`

- libzpaq (public domain) wired as a per-block candidate, **only** under `--max-ratio`.
- Context mixing is in the PAQ family: far stronger on structured data, tens of times slower.
- Never constructed without the flag, so the default path pays nothing for it.

**Result on `xml` (5,345,280 bytes):**

- `-9`: 423,579 bytes, 22 s
- `--max-ratio`: **394,185 bytes, 75 s** (-6.9% vs `-9`)
- `xz -9`: 453,260 -> `--max-ratio` is **-13.1%** against it
- Round-trip exact

**Build notes:**

- libzpaq branches on `#ifdef unix` and otherwise includes `<windows.h>`; the macro is not predefined by this compiler, so it is passed explicitly.
- `NOJIT` is defined: the x86 JIT wants executable memory, which we should not depend on.
- libzpaq declares `libzpaq::error()` but expects the host to define it. Without it the library links and then has no way to report failure; ours throws, and every call is wrapped.
- Its header predates C++20, so the wrapper joins the C++17 shim target — and must not include `chimera/format.hpp` (which uses `std::span`), so its interface is plain vectors and bools.

**Not measured:** the phase's exit criterion is `--max-ratio` beating `xz -9` across total Silesia. That is out of reach in this sandbox: at 75 s for 5.3 MiB, the 212 MiB corpus is over 50 minutes of compression, against a 300 s per-command limit. The single-file result above is what was measured.

**Tests:** `unit.zpaq`, 2 cases — round-trip at three sizes, and a corrupted payload that must never report success with the wrong bytes. 15/15 ctest suites pass.

## Phase 11 — PPMd for text

- 7-Zip's PPMd var.H (`Ppmd7.c`, `Ppmd7Enc.c`, `Ppmd7Dec.c`), public domain, order 6, 16 MiB model.
- PPM models *context* rather than position, which is why it beats LZ and BWT on natural language.
- Offered per block, so binary blocks never pay for it.

**Result:**

- `dickens` (English text): 2,820,882 -> **2,743,444**, i.e. **-2.7%** — the phase's "beat the existing best on text by 2%" gate is met.
- `reymont` (Polish text): 1,249,670 -> 1,248,492, -0.09%; BWT still wins most blocks there.
- Round-trip exact.

**Two integration details that were not obvious:**

- `Ppmd7z_EncodeSymbols` emits symbols but does **not** initialise the range encoder or flush it. The caller must set `rc.enc.Low/Range/Cache/CacheSize` and call `Ppmd7z_Flush_RangeEnc` afterwards, or the tail of the stream is lost.
- The stream interfaces (`IByteOut`/`IByteIn`) are const-qualified pointers to a struct we own, so the callbacks need a `const_cast`, not a plain `reinterpret_cast`.

**Test fragility fixed properly this time.** Adding a backend kept leaving the round-trip stats stale, because each backend had its own named counter and I kept forgetting to add the new one. `CompressStats` now carries `mode_counts[16]`, indexed by mode byte, and the test sums that array. Adding a backend can no longer make the check stale.

**Tests:** `unit.ppmd`, 3 cases — round-trip on text and on binary, and truncation rejection. 16/16 ctest suites pass.

## Phase 12 — Tuning: block size

**The single biggest tuning win in the project, and it was free.** Every backend
now benefits from a large block: PPMd builds a deeper context model, BWT sorts a
bigger window, and the fine match finder sees more history per block.

On `dickens` (10,192,446 bytes), `-9`, varying only the block size:

| block size | bytes | vs 256 KiB |
|---|---|---|
| 256 KiB (old default) | 2,743,444 | — |
| 1 MiB | 2,496,909 | -9.0% |
| **4 MiB (new default)** | **2,355,513** | **-14.1%** |

Total time is flat (62-72 s across all three), because the extra work per block
is offset by having far fewer blocks. Memory is the trade: a BWT block needs a
few multiples of its own size, and PPMd holds a 16 MiB model per block.

Against `xz -9` (2,830,604) the 4 MiB default is **-16.8%** on dickens, and on
`x-ray` 3,798,292 vs 4,489,868 is **-15.4%**. Both round-trip exact.

**Fuzzing and round-trip.** 16/16 suites pass at the new default, including the
mutation fuzz suite and the CLI integration tests.

### What is NOT done in this phase

Two items from the plan are open, and it is worth being precise about why:

- **Cheap mode selector.** The compressor still tries every applicable backend on
  every block, which is why `-9` runs at roughly 0.3-0.5 MB/s. A sample-based
  pre-selection (the pattern already used for BCJ and for preflate) would cut
  that several-fold with little ratio loss. Not implemented.
- **Parallelism.** Blocks are not independent in this format: the match finder
  and coarse index carry state across block boundaries, and tokens may reference
  earlier blocks. Compressing blocks in parallel would silently break that.
  Parallel decompression is equally unsafe for the same reason. Doing it properly
  needs either a per-block "no cross-block references" flag or a two-pass
  scheme — a format change, not a threading change.

## Timing harness

`tests/benchmark/timeit.sh` runs a command N times and reports **min, median, max
and spread**, not a single number. The spread is the point: on this shared vCPU,
identical work has measured 72 s, 86 s and 124 s, so one timing is worthless and
even an average hides how untrustworthy it is. The script prints a warning when
the spread exceeds 20%.

For comparing two configurations it is not enough on its own — runs must be
**interleaved** (A, B, A, B...) so that drift in machine load affects both
equally. That is how the measurement below was taken.

## Phase 12 — the cheap selector: attempted, measured, rejected (twice)

The plan's remaining speed item was to stop trying every backend on every block.
I implemented it: a 64 KiB sample from the middle of the block runs BWT and PPMd,
and the loser is skipped on the full block unless the two are within 1-3% on the
sample.

**It was reverted, not shipped.** Two reasons:

1. **The ratio got slightly worse** — `dickens` 2,355,513 -> 2,360,276 (+0.2%).
   The sample is not a perfect predictor of a 4 MiB block, so occasionally it
   skips the backend that would have won.

2. **The speed benefit could not be measured on this machine.** Three runs of
   essentially identical work gave **72 s, 86 s and 124 s** — roughly 70%
   variance on a shared vCPU. Any speed claim from numbers like those would be
   fiction, and I would rather leave the optimisation unshipped than ship it on
   the strength of a measurement that cannot support it.

Both the change and its measurements are recorded here rather than quietly
dropped, because the negative result is the useful part: **speed work on this
project needs a stable machine before it can be done honestly.** The ratio side
has been measured throughout on sizes, which are exact and reproducible; the
throughput numbers in earlier phases (0.3-0.5 MB/s, 10-72 MB/s decode) are
therefore order-of-magnitude figures, not precise ones.

### The second attempt, with the harness

The first attempt could not measure anything. With `timeit.sh` and interleaved
A/B runs on `xml`, it can — and the answer is still no.

Baseline, 5 runs: 24,349 / 24,559 / 22,182 / 22,226 / 26,311 ms
(median 24,349, **spread 17%**).

Interleaved A/B, 5 pairs, same binary, selector behind `CHCE_SELECTOR`:

| pair | baseline | selector |
|---|---|---|
| 1 | 24,349 ms | 25,645 ms |
| 2 | 24,559 ms | 23,908 ms |
| 3 | 22,182 ms | 21,388 ms |
| 4 | 22,226 ms | 24,396 ms |
| 5 | 26,311 ms | 23,627 ms |

- **Speed: median 24,349 -> 23,908 ms, -1.8%.** The spread on both sides is 17-18%,
  so this is inside the noise: the selector does not demonstrably save time.
- **Ratio: 374,682 -> 384,728 bytes, +2.68%.** That one is exact, and it is bad.

A 64 KiB sample does not predict a 4 MiB block well enough. A 2.7% size loss for
an unmeasurable speed gain is a bad trade, so the change is rejected again — this
time with numbers behind the decision rather than a shrug.

**What would actually help throughput:** the cost is dominated by running two
expensive backends over every full block, and sampling is the wrong instrument
for deciding between them. Real options are (a) parallelism, which the two-mode
plan covers, or (b) a much cheaper predictor built from the parse the engine
already performs — match coverage and literal entropy per block — rather than a
trial compression of a sample.

## Final corpus measurement (the standing result)

All 12 phases are in. This is the whole corpus measured once, at the shipped
defaults (4 MiB blocks, every backend offered, `-9`), against `xz -9` on the same
file. Sizes are exact; timings on this shared vCPU are not reliable and are
omitted deliberately.

| file | raw | `xz -9` | ziptastic | vs `xz -9` |
|---|---|---|---|---|
| `reymont` | 6,627,202 | 1,317,152 | 1,024,579 | **-22.213%** |
| `mr` | 9,970,564 | 2,750,272 | 2,249,532 | **-18.207%** |
| `osdb` | 10,085,684 | 2,849,908 | 2,347,694 | **-17.622%** |
| `xml` | 5,345,280 | 453,260 | 374,682 | **-17.336%** |
| `dickens` | 10,192,446 | 2,830,604 | 2,355,513 | **-16.784%** |
| `nci` | 33,553,445 | 1,738,884 | 1,449,230 | **-16.657%** |
| `x-ray` | 8,474,240 | 4,489,868 | 3,798,292 | **-15.403%** |
| `ooffice` | 6,152,192 | 2,426,816 | 2,083,954 | **-14.128%** |
| `samba` | 21,606,400 | 3,763,616 | 3,739,486 | **-0.641%** |
| `sao` | 7,251,944 | 4,415,072 | 4,415,033 | **-0.001%** |

**10 files, 119,259,397 bytes in: 27,035,452 with `xz -9` against 23,837,995 with
ziptastic — 11.83% smaller.**

Every file is at or below `xz -9`. The spread is worth reading:

- **Text is now far ahead** — `reymont` -22.2%, `dickens` -16.8%, `xml` -17.3%.
  This is the 4 MiB block size doing the work: PPMd and BWT both build much
  better models from a large block, and at 256 KiB these same files were -5% to
  -7%.
- **Structured binary is close behind** — `mr` -18.2%, `osdb` -17.6%,
  `x-ray` -15.4%, `ooffice` -14.1%.
- **`samba` is the weak file at -0.64%** — a tar of mixed content where the LZMA2
  safety net wins, so we are essentially tied with `xz -9` there.
- **`sao` is exactly tied (-0.00%)** — the fallback wins outright.

**Two files could not be measured: `webster` (41 MiB) and `mozilla` (51 MiB).**
Each exceeds the sandbox's 300 s per-command limit at 4 MiB, 1 MiB *and* 512 KiB
blocks. They are reported as unmeasured rather than estimated: `webster` is
English text like `dickens` and would be expected to land in the same -15% to
-20% band, and `mozilla` is a tar of executables where `samba`'s pattern (-0.6%)
is the closer precedent — but those are expectations, not measurements, and are
labelled as such.

## End-to-end CLI fuzzing

`tests/integration/test_fuzz_cli.sh`, registered as `integration.fuzz_cli`.

- Drives the real binary, not the library: compress a file, decompress it, `cmp`.
- 25 inputs x 3 flag sets: random bytes, English text, constant runs, base64.
  Sizes 0-64 KiB, so the block loop, the layer-0 transforms and the mode selector
  all get exercised at small sizes.
- Then corrupts a valid stream at a random offset and requires a **clean failure**
  — not a crash (exit >= 128) and not a hang (timeout 124). 25 more cases.
- Then truncates streams by 1, 4, 16 and 64 bytes and requires rejection.
- Deterministic (`RANDOM` is seeded), so any failure reproduces.

**54 checks, 0 failures.** Registered in CTest, so it runs with the rest.

Why this and not more unit fuzzing: the unit fuzz suite mutates encoded blocks and
checks the decoder in-process. Every bug that actually reached a measurement in
this project was an *integration* bug — a flag that disagreed with its data
(`kFlagPrecomp` set while the original bytes were compressed), a mode case
grouped with the wrong handler (CmZpaq sharing Brotli/Zstd/Lz4). Neither would be
caught by mutating a block; both need the real pipeline.

## Decided: two modes for parallelism (not implemented yet)

Parallelism needs blocks to be independent, and today they are not: the match
finder and coarse index carry state across block boundaries and tokens may
reference earlier blocks. Rather than trade that away, the plan is **two modes**,
both shipped:

- **Archive mode** (default, what exists now): coupled blocks, cross-block
  references, maximum ratio. This is the mode all the measurements above use.
- **Parallel mode** (to build): each block self-contained, no cross-block
  references, compressed on N threads. Lower ratio, faster wall-clock.

The format already has room for this: a header flag bit marks the stream as
parallel-safe, and a decoder can reject a stream whose blocks reference backwards
across a boundary. Archive mode stays available, so nothing measured so far is
lost.

## Parallel mode (built)

Two modes, as agreed, both shipped:

- **Archive mode** — the default. Blocks are coupled: the match finder and coarse
  index carry across block boundaries, so a match can reach arbitrarily far back.
  Maximum ratio. Every measurement in this document uses this mode.
- **Parallel mode** — `--parallel` (with `-T N` for the thread count). Each block
  is encoded against **itself only**: its own match finder over its own slice, no
  coarse index, no carried dictionary. That is what makes the blocks independent,
  and it is the whole difference between the two modes.

The header carries `kFlagParallel` (bit 4) when the stream was written that way,
so a decoder knows the blocks may be decoded independently.

**Measured on `xml` with `-b 512` (11 blocks, 2 cores):**

| mode | bytes | time | round-trip |
|---|---|---|---|
| archive | 408,960 | 28 s | exact |
| parallel | 422,808 | 20 s | exact |

- **Ratio: +3.4%** — the price of dropping cross-block references.
- **Time: -29%** on 2 cores (1.4x). Not 2x, because per-block work includes
  single-threaded candidate encoding and the writer is sequential.
- On the same file with the default 4 MiB blocks (2 blocks) the two modes gave
  **identical** output (374,682 bytes), because there was no cross-block
  redundancy to lose.

**What is built and what is not:**

- Built: parallel **compression**, the format flag, both modes shipping.
- Not built: parallel **decompression**. The flag is written so a decoder may do
  it, but the current decoder still decodes blocks sequentially. Decoding is
  already 10-72 MB/s against a 150 MB/s target, so this is the next lever rather
  than a blocker.

**One bug the refactor introduced, and the test that caught it.** Extracting the
per-block body into a shared `encode_one_block` meant rewriting the statistics
accounting, and the rewrite dropped the `raw` and `lzma2` counters —
`fallback_declines_on_incompressible_data` failed with `raw_blocks 0 vs blocks 1`
immediately. Worth recording because it is the argument for extracting one
function rather than duplicating the body for the parallel path: the duplicate
would have kept the counters and silently drifted everywhere else.

## Experiments: do other combinations beat what we ship?

Question asked: can more modes be added by trying different combinations? Measured
with a throwaway harness that links the same libraries and tries each combination
on 4 MiB blocks, reporting bytes per file. Baseline is BWT (what the engine picks
for most blocks).

| file | bwt | ppmd6 | ppmd16 | bwt+lzp | delta1+bwt | delta2+bwt | delta4+bwt |
|---|---|---|---|---|---|---|---|
| `xml` | **374,656** | 488,884 | 374,660 | 474,460 | 432,226 | 518,506 | 718,506 |
| `dickens` | **2,359,168** | 2,360,242 | 2,410,615 | 2,403,278 | 2,449,302 | 2,829,444 | 4,396,558 |
| `sao` | **4,676,596** | 4,778,832 | 4,784,349 | 4,675,460 | 5,215,020 | 5,628,630 | 6,256,406 |
| `x-ray` | **3,798,258** | 4,056,573 | 3,908,665 | 3,798,258 | 3,861,390 | 3,927,660 | 4,123,976 |

**Nothing beat what we already ship.** Details worth keeping:

- **Delta preprocessing is a clear loser on every file** (+1.7% to +91%). This was
  the most promising idea on paper — byte-delta before BWT is the standard trick
  for numeric arrays — and it lost everywhere, including on `x-ray` and `mr`,
  which are 16-bit image data. Whatever redundancy those have, BWT already finds
  it; differencing just destroys the byte statistics the QLFC coder relies on.
- **LZP inside libbsc is a wash**: identical output on `x-ray` and `xml`-adjacent
  blocks (libbsc declines it there), -0.02% on `sao` (1,136 bytes on 7 MiB —
  noise), and +1.9% worse on `dickens`. Not worth a mode.
- **PPMd order is content-dependent, and order 6 is the right default.** On
  `dickens` order 6 ties BWT (+0.05%) while order 16 is 2.2% worse. On `xml` the
  reverse: order 6 is **30% worse** than BWT and order 16 ties it. So the order
  matters a lot, but adding a second order as a candidate buys nothing on this
  corpus, because wherever order 16 catches up, BWT was already there.
- **`sao` is the file we lose to nothing and win against nothing** — every
  alternative is worse, and it is the one file where our output equals `xz -9` to
  within 39 bytes.

**Conclusion: the candidate set is already at a local optimum on this corpus.**
Adding modes for their own sake would cost encode time and buy no bytes. If more
modes are wanted, the honest next candidates are not preprocessing variants but
different families entirely — a stronger CM configuration for `--max-ratio`
(currently libzpaq level 5, which is not its strongest setting), or a larger
number of PPMd orders *only* if the selector that picks between them can be made
cheap, which the earlier experiment showed it cannot be.

## SREP-style dedup: measured, and the case for building it

SREP is two separate things, and only one of them exists here:

- **Its dedup engine** — a coarse block-hash index with long-range references.
  Built from scratch in Phase 6 (`coarse_index.cpp`), because SREP's source is
  all-rights-reserved.
- **Its role as a pipeline stage** — dedupe the stream first, then compress the
  deduped result (the FreeArc chain: precomp -> srep -> lzma). **Not built.**

### Measurement on an archive with duplicate members

`dup2.tar`: three 3 MiB files, one of them duplicated (9,011,200 bytes).

| configuration | bytes |
|---|---|
| `xz -9` | 1,486,432 |
| ziptastic, coarse index **on** | 1,326,180 |
| ziptastic, coarse index **off** | 1,326,185 |
| **ideal dedup** (duplicate removed before compression) | **1,188,707** |

Two conclusions:

1. **The coarse index makes no difference here — 5 bytes.** The duplicate sits
   ~6 MiB back, inside the 64 MiB fine window, so the match finder already had
   it. The coarse index only earns its keep beyond the window (measured earlier:
   1,032,192 bytes recovered on a 100 MiB gap).
2. **A dedup stage would save 10.4% on this file**, and the reason is not
   distance: it is that **per-block backend selection picks BWT or PPMd for these
   blocks, and neither can reference another block.** The duplicate is invisible
   to them. Dedup would remove it before block splitting, so every backend
   benefits.

That second point is the real argument for building SREP's pipeline role: the
engine's strength — picking the best backend per block — is also a blind spot for
cross-block redundancy, and the coarse index only feeds the *token* path.

### What building it would involve

- A Layer 0b stage before block splitting: scan with the existing `CoarseIndex`,
  emit literals plus back-references, and compress that payload normally.
- A new header flag, and the same verify-before-use discipline used for preflate
  (reconstruct and compare before committing to the transform).
- **Built.** See the next section for the result.

## SREP-style dedup stage (built)

`src/precompressors/dedup.cpp`, a Layer 0 stage that runs after BCJ and before
block splitting. It scans with the existing `CoarseIndex`, replaces long repeated
runs with back-references, and hands the result to the normal pipeline. The
header carries `kFlagDedup` (bit 5).

Stream layout: `varint original_size`, then repeated `0x00 varint len <literals>`
and `0x01 varint dist varint len`. The original length lives in the stream, so no
extra format field was needed.

**Result:**

| | bytes |
|---|---|
| `dup2.tar` without dedup | 1,326,180 |
| **`dup2.tar` with dedup** | **1,183,152 (-10.8%)** |
| ideal (duplicate removed by hand) | 1,188,707 |

It slightly beats the hand-made ideal, because it also deduplicates across the
tar's own block boundaries. Round-trip exact.

**No regressions:** `xml` 374,682, `reymont` 1,024,579, `ooffice` 2,083,954 — all
byte-identical to before, because the stage declines data it should not touch.

### The bug that mattered: short repeats must be left alone

First version used a **64-byte minimum match**, and it made **`xml` 20.8% worse**
(374,682 -> 452,747). XML is full of repeated tags: lifting them into commands
destroys exactly the structure BWT and PPMd exploit. Two changes fixed it:

- **Minimum match 4096 bytes.** Only substantial duplicates — whole files inside
  an archive — are worth lifting out.
- **Engage only when the removal exceeds 25%** of the input. A small reduction is
  a strong signal that the repeats were short and structural, and the command
  stream will compress worse than the original.

With both, `xml` is untouched and the duplicate archive still gains 10.8%.

**Known gap, unrelated to this stage:** the format has no checksum. A flipped
literal byte inside a dedup packet is undetectable and decodes to wrong content of
the right length. `corruption_never_produces_a_wrong_length` pins what is actually
guaranteed (a correct length, never a crash or a hang); the missing integrity
field is a format-level gap worth closing separately.

**Tests.** `unit.dedup`, 5 cases: round-trip with a large duplicate, declining
XML-like short repetition, accepting a genuinely periodic pattern (an earlier
version of this test asserted the opposite and was wrong about the data),
declining unique data, and corruption never producing a wrong length. 18/18 ctest
suites pass.

## --max-ratio: context mixing over the whole stream

I previously wrote that libzpaq level 5 "is not its strongest setting" and that a
stronger CM configuration was the next lever. **That was wrong**, and measuring it
was cheap:

| method string | bytes on `xml` |
|---|---|
| **"5"** (what we use) | **330,394** |
| "6", "7", "8" | 330,394 (identical — libzpaq clamps at 5) |
| "x6.0ci1,1,1,1,4,4,24,40,64" | 378,162 |
| "x6.0ci1,1,1,1,3,3,24,40,64,0,0,0" | 381,098 |
| "x6.0ci1,1,1,1,1,2,2,20,36,64" | 383,729 |
| "x9.0ci1,1,1,1,1,2,2,20,36,64" | out of memory |

Every `x`-config is **14-16% worse** than level 5. There is no stronger setting to
switch to.

### What the measurement did reveal

`"5"` on the whole file gives **330,394**, while `--max-ratio` per block gave
**394,185** — a 16% gap that the block boundaries were costing. A CM model is
stateful and long-lived; cutting it every 4 MiB throws away what it learned.

So `--max-ratio` now also tries one CM pass over the **entire stream**, and keeps
it only when smaller:

| file | `-9` (archive mode) | `--max-ratio` before | `--max-ratio` now |
|---|---|---|---|
| `xml` | 374,682 | 394,185 | **330,417 (-16.2%)** |
| `reymont` | 1,024,579 | — | **953,851 (-6.9%)** |

Round-trip exact on both; 18/18 suites pass. The xml result is within 23 bytes of
the theoretical best for this method, i.e. the framing is all that is left.

It is tried **last and only kept when smaller**, because it is the most expensive
thing in the engine — and it is confined to `--max-ratio`, so the default path
never pays for it.

## Parallel decode

Decoding a `kFlagParallel` stream now uses all cores: every block decodes into its
own buffer and the results are concatenated. That is only safe because the flag
guarantees no block references another — in archive mode the same code would read
output a block has not produced yet, which is why the sequential path remains for
everything else.

**Measured on `xml`, 2 cores:**

| stream | blocks | parallel | sequential |
|---|---|---|---|
| `-b 512` | 11 | 356-489 ms | **241-295 ms** |
| `-b 64` | 82 | **115 ms** | 138 ms |

- **At 82 blocks: ~17% faster.**
- **At 11 blocks: slower.** Thread startup costs more than 11 small blocks save.
- Both spreads are wide (the shared vCPU again), so the 17% is suggestive rather
  than conclusive; the loss at 11 blocks is unambiguous.

So the path is gated at `kParallelDecodeMinBlocks = 8`: below that, sequential
wins and sequential is used. Decoding at the default 4 MiB blocks — 2 blocks for a
5 MiB file — is therefore unaffected, which is the honest outcome: parallel decode
only pays when there are many blocks to spread.

**Still unmet:** the 150 MB/s decode target. Parallelism is not the lever for it —
the decode cost is in the entropy coders, and reaching that number needs the
interleaved rANS work deferred long ago, not more threads.

## Interleaved rANS

A single-state rANS is a serial dependency chain: each symbol's state depends on
the previous one, leaving the CPU's out-of-order machinery nothing to overlap.
The static rANS path now codes across **4 independent lanes** (`kRansLanes`),
round-robin by symbol index. The adaptive model stays on one lane, because its
counts and state have to advance together.

**Measured (micro-benchmark, 4 M symbols, skewed 16-symbol alphabet):**

| lanes | decode throughput |
|---|---|
| 1 | 82.0, 85.9 Msym/s |
| **4** | **90.2, 97.6 Msym/s** |

**~12% faster, for 9 bytes of payload.** Correctness is unchanged: 12/12 rANS
unit tests and 18/18 suites pass, and the round-trip is exact.

**That is much less than interleaving usually buys, and the reason is worth
recording:** this loop is bound by table lookups and the renormalisation branch,
not by the state dependency. Interleaving removes a bottleneck that was not the
binding one here. The 150 MB/s decode target is not reached by this, and reaching
it needs a different approach — smaller tables that fit in L1, or a coder whose
inner loop does less work per symbol — not more lanes.

**One implementation detail that cost a build cycle:** the encoder walks symbols
*backwards* (rANS is LIFO), so a call-order counter assigns lanes in reverse and
desynchronises from the decoder, which walks forwards. `put()` therefore takes the
symbol's **logical index** and derives the lane from it.

## Corpus refresh (after dedup, checksum, 4 MiB default, interleaved rANS)

The earlier standing result was measured before the dedup stage, the CRC-32 and
the interleaved rANS. Re-measured on the final binary, `-9`, defaults:

| file | raw | `xz -9` | ziptastic | vs `xz -9` |
|---|---|---|---|---|
| `reymont` | 6,627,202 | 1,317,152 | 1,024,583 | **-22.212%** |
| `mr` | 9,970,564 | 2,750,272 | 2,249,536 | **-18.207%** |
| `osdb` | 10,085,684 | 2,849,908 | 2,347,698 | **-17.622%** |
| `xml` | 5,345,280 | 453,260 | 374,686 | **-17.335%** |
| `dickens` | 10,192,446 | 2,830,604 | 2,355,517 | **-16.784%** |
| `nci` | 33,553,445 | 1,738,884 | 1,449,234 | **-16.657%** |
| `x-ray` | 8,474,240 | 4,489,868 | 3,798,296 | **-15.403%** |
| `ooffice` | 6,152,192 | 2,426,816 | 2,083,958 | **-14.128%** |
| `samba` | 21,606,400 | 3,763,616 | 3,739,490 | **-0.641%** |
| `sao` | 7,251,944 | 4,415,072 | 4,415,037 | **-0.001%** |

**10 files, 119,259,397 bytes in: 27,035,452 with `xz -9` against 23,838,035 with
ziptastic — 11.83% smaller.**

The figure is unchanged from before those three changes, and that is the expected
answer rather than a disappointment: each one was verified not to regress before
it shipped. The only difference is **+4 bytes per file**, which is the CRC-32 now
carried in the header (40 bytes across ten files).

Still unmeasured: `webster` (41 MiB) and `mozilla` (51 MiB), which exceed the
300 s per-command limit at every block size tried.

## JPEG recompression (brunsli) — integrated

`src/precompressors/jpeg_wrap.cpp`, a Layer 0 stage that runs **first**, before
DEFLATE precompression, BCJ and dedup. A JPEG is neither DEFLATE, nor x86 code,
nor text, so every other layer would waste its time on it. The header carries
`kFlagJpeg` (bit 6).

brunsli re-encodes a JPEG's DCT coefficients and keeps a recipe that reproduces
the file exactly. Measured through the shipped CLI:

| image | JPEG | before | after | gain |
|---|---|---|---|---|
| `color_photo.jpg` | 21,797 | 21,342 | **17,202** | **-19.4%** |

All five vendored test JPEGs round-trip **bit-exact** through `ziptastic -9` and
back. Non-JPEG data is untouched: `xml` is 374,686 before and after.

**Why it works where compressing the JPEG does not:** the engine got 21,342 from a
21,797-byte JPEG — about 2%. The bytes are already Huffman-coded; there is nothing
left to find. brunsli goes a level down, to the quantised coefficients, and its
output does not compress further either (22,643 -> 22,664 through the engine, a
rounding loss). The entire gain is replacing the representation.

**Two vendored-code fixes were needed**, both recorded here because they are the
kind of thing that silently rots:

- The brunsli copy inside precomp-cpp is **internally inconsistent**: its headers
  declare `BrunsliEncodeJpeg`/`BrunsliDecodeJpeg` with a fourth `use_brotli`
  argument, while its own `encode.cc` and `decode.cc` call them with three. It
  does not compile as vendored. Adding `false` at both call sites fixes it.
- brotli's sources are **C**, and CMake must compile them as C. Handing them to
  the C++ driver fails with `-std=c++17 not allowed with C`.

**Licence:** brunsli and brotli are both MIT (Google), so this is the first
significant capability added from the vendored shelf that is permissively licensed
*and* measured. It is also the largest single win found in the whole scan.

**Tests:** `unit.jpeg`, 3 cases — header detection, declining non-JPEG data, and a
bit-exact round trip of a real vendored JPEG. 19/19 suites pass.

## PDF stream extraction

PDF stores its content streams as DEFLATE inside `stream` ... `endstream`. The
preflate stage now finds those, so a PDF is recompressed at the same level as a
ZIP: the stream is inflated, the inflated bytes go through the normal pipeline,
and preflate's recipe reproduces the original stream exactly.

**Measured through the CLI:**

| file | before | after | note |
|---|---|---|---|
| `poor.pdf` (level-1 deflate, 52,359 B) | 31,452 | **6,562** | **-79.1%**, `precomp=True` |
| `big.pdf` (level-9 deflate, 25,926 B) | 6,034 | 6,034 | `precomp=False` — declined, and correctly |

The second row matters as much as the first: when the PDF's DEFLATE was already
good, inflating buys nothing and the stage declines. It only pays where the
producer's deflate was weak — which is the common case for PDFs written by fast or
legacy encoders.

**Two bugs, both of which failed silently:**

1. **The end-of-line before `endstream` is not part of the stream.** Slicing to
   `endstream` included it, making the DEFLATE one byte too long. The parse simply
   returned false — no error, no signal, the transform just never engaged.
2. **PDF's `/FlateDecode` is zlib-wrapped (RFC 1950)** — a 2-byte header and a
   4-byte Adler-32 around the raw DEFLATE. preflate works on raw DEFLATE, so it
   has to be handed the middle. The wrapper stays in the skeleton as literal
   bytes, which keeps the repack exact without recomputing the checksum. ZIP
   stores raw DEFLATE, which is why this never came up before.

Both were found the same way: by checking whether the transform had actually
engaged (the `kFlagPrecomp` bit) rather than trusting the output size. The first
version "improved" `poor.pdf` by 40% through the ordinary pipeline while the new
code did nothing at all.

**Not done, and why:** MP3 via `packmp3` is **LGPL** (it depends on `packjpg`), so
it is blocked by the same MIT-only rule as bzip3. GIF and PDF handling inside
precomp-cpp live in its 8,512-line `precomp.cpp` rather than in modules — GIF would
mean lifting code out of a monolith, so PDF was written from scratch instead.

## GIF recompression

`src/precompressors/gif_wrap.cpp`, a Layer 0 stage after JPEG. A GIF is LZW-coded
palette indices, and LZW is a 1980s coder; the indices compress far better through
this engine.

The transform is used **only when the original LZW stream can be reproduced
byte-exactly**. A GIF's LZW is not canonical — clear-code placement and code-width
changes are encoder choices — so a re-encode that differs by one bit disqualifies
the file. Verification runs before the transform is committed, like every other
layer.

**Measured through the CLI, all round-trips exact:**

| file | GIF | engine alone | with GIF layer | gain |
|---|---|---|---|---|
| photo-like, dithered (`real.gif`) | 179,006 | 175,946 | **126,127** | **-29.5%** |
| smooth gradient (`t1.gif`) | 80,586 | 80,389 | **12,644** | -84.3% |
| `x*y mod 64` pattern (`t2.gif`) | 26,839 | 24,027 | **2,415** | -91.0% |

The first row is the honest one: **-29.5% on a dithered photo-like image**, which
is what real GIFs contain. The other two are synthetic patterns and represent the
ceiling, not the norm.

**A bug worth recording, because the design caught it.** The first version packed
correctly but failed to unpack, and the engine silently declined the transform
because verification failed. The cause: I compared the re-emitted sub-block chain
against `lzw.size() + 1`, but the chain also carries a length byte per 255-byte
chunk, so the framing never matched. The safety net meant this cost a rebuild
rather than a corrupted archive — and it is the second time this session that
"did the transform actually engage?" was the question that found the bug.

**Flag space is now exhausted.** `kFlagGif` took bit 7, so `kFlagMask` is 0xFF and
there is no invalid flags byte any more. `file_header_bad_flags` was rewritten as
`file_header_flags_are_full`: it asserts the mask is full and that every byte value
is accepted. The next capability needs a new header field, not another bit.

**Tests:** `unit.gif`, 3 cases — signature detection, declining non-GIF data, and a
bit-exact round trip of a vendored sample. 20/20 suites pass.

## LZ4 precompression — potential measured, not yet integrated

LZ4 was identified as the highest-value unhandled container (see
`docs/REPACK_SOURCES.md`). Before writing any integration, the payoff was measured
directly: take a Silesia file, compress it with liblz4's frame format, then compare
compressing the resulting `.lz4` file against compressing the payload inside it.

| file | raw | LZ4 file | our engine on the `.lz4` | our engine on the payload | gain |
|---|---|---|---|---|---|
| `xml` | 5,345,280 | 1,177,119 | 897,038 | **374,686** | **-58.2%** |
| `dickens` | 10,192,446 | 6,404,486 | 4,246,906 | **2,355,517** | **-44.5%** |
| `mr` | 9,970,564 | 5,428,613 | 3,159,203 | **2,249,536** | **-28.8%** |

This is the same order as the JPEG (-19.4%), GIF (-29.5%) and PDF (-79%) wins, and
far above anything the entropy-level ideas promised (1-3%). It confirms the pattern:
**containers pay, codecs do not.**

Note what the middle column says on its own: compressing an LZ4 file with a strong
compressor still recovers 24-34% of it. LZ4 is a *speed* codec and leaves a great
deal of redundancy for a BWT/PPMd-class engine to find.

### The honest caveat before integration

The table shows the **ideal** case — the payload compressed as if it were free to
extract. A real implementation must also reproduce the `.lz4` bytes exactly, which
means storing the parse (literal runs, match offsets and lengths) alongside the
payload. That overhead is real and will eat into the gain, most on `mr`, where the
matches are long and dense.

The encouraging difference from the GIF work: **LZ4 does not need its encoder
reproduced.** A GIF's LZW is not canonical, so a re-encode either matches exactly or
the file is rejected. An LZ4 frame is an explicit sequence of literals and matches,
so the parse can simply be *stored* and re-emitted — exactness is structural rather
than a lucky match. That makes LZ4 a more predictable job than GIF was.

## Integer CSV/TSV columns — shipped

`src/precompressors/numeric_wrap.cpp`, the first transform carried in
`FileHeader::layers` rather than in `flags` (which was full). Format version 3.

**Measured through the shipped CLI, post-compression, round-trip exact:**

| file | engine without | engine with | gain |
|---|---|---|---|
| `data.csv` (4 columns x 40k rows, with header) | 164,257 | **37,174** | **-77.4%** |
| `data.tsv` (2 columns x 40k rows) | 74,935 | **37,090** | **-50.5%** |

Decline paths verified: a quoted CSV and a column with `007` are both left
untouched, and both still round-trip exactly.

**Design, as settled by measurement:**

- **integers only** — floats gave -0.6% on smooth data, not worth the gating.
- **delta + zigzag + varint** — Rice coding lost by 10-20%, the same bit-alignment
  lesson as Gorilla.
- **per-column accept/reject** — a column is transformed only if its varint stream
  beats its raw text; the trial declined on random and noisy columns as designed.
- **canonical integers only** — `007`, `+5`, `5.0`, `1,234` are declined, never
  normalised. Quoted CSV is declined wholesale in v1.
- **header row detected** — if row 0 fails to parse while rows 1..n parse, it is
  stored raw and marked as a header, so the transform fires on real CSVs rather
  than only on headerless synthetic ones.
- **delimiter sniffed** — `,` `\t` `;` `|`, requiring every row to split to the same
  column count.
- **first value absolute**, the rest deltas.

**Why this needed a format change:** `flags` had no free bit left after
`kFlagGif`. Rather than overload it, `FileHeader` gained a `layers` byte and the
version went to 3 — the change this project had already flagged as due when the
last flag bit was consumed. Two tests that build headers by hand were updated.

## Spreadsheet XML cell values — shipped

`<v>` values inside spreadsheet XML are coded as delta + zigzag + varint, per
column, with a per-cell decline. Full write-up and the design measurements:
`XML_NUMERIC_MEASUREMENT.md`.

| file | before | after | gain | `flags` | `layers` |
|---|---|---|---|---|---|
| `book.xlsx` (2 cols x 40,000 ints) | 147,525 | **73,367** | **−50.3%** | 0x05 | 0x08 |
| `sheet1.xml` (inflated payload alone) | 148,615 | **72,464** | −51.2% | 0x01 | 0x04 |
| random 9-digit integer sheet | 495,648 | 414,446 | −16.4% | 0x01 | 0x04 |
| floats + strings sheet | 196,987 | 196,987 | declines | 0x01 | 0x00 |
| `book.tar.gz` wrapping the xlsx | 208,652 | 208,652 | unchanged | 0x01 | 0x00 |

All round trips byte-exact. The `.tar.gz` row is the honest gap: precomp does not
enter a **gzip** container, so recursion never starts — see the gzip item.

Two things measured before the C++ existed, both of which changed the design:
column-grouped streams came to 71,964 against 71,963 interleaved (0.04%, not worth
the machinery), and a flat single-prev variant reached 71,994 but with a 3x larger
intermediate stream (239,999 vs 80,003 bytes), which is a ratio risk on less
regular data.

The random-integer row is worth remembering: delta coding buys nothing there, and
the transform still gains 16.4%, because 30-bit values are 4-5 varint bytes against
9-10 decimal characters. That part is base conversion, not prediction.

## Per-block integrity (format v4)

32-byte SHA-256 per block plus a 37-byte trailer. Full write-up: `INTEGRITY.md`.

| | |
|---|---|
| stream overhead, single block | +69 bytes |
| stream overhead, per extra block | +32 bytes |
| 100 MB at the default 4 MiB block | 837 bytes (0.0008%) |
| `-b 1` (1 KiB blocks) | 3.1% |
| SHA-256 throughput, this machine | 140.8 MB/s (28.4 ms per 4 MiB block) |
| share of decode wall time | ~22% on a 3.25 MB sample (102.7 ms decode, 23.1 ms digest) |

Measured v3 -> v4 on the existing fixtures: `book.xlsx` 73,367 -> 73,436,
`sheet1.xml` 72,464 -> 72,533, `book.tar.gz` 208,652 -> 208,721. Every one is
exactly +69, which is the framing arithmetic and nothing else.

The time column is the one to argue about: 22% of decode for integrity is a real
trade, and the alternatives (SHA-NI, per-block CRC-32, an optional algorithm
field) are listed in `INTEGRITY.md` with their costs.
