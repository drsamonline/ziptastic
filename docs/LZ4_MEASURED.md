# LZ4 precompression: measured, and the parse is not free

Written 2026-11-02. This is a **negative result**. `docs/BENCHMARKS.md` recorded
LZ4 as the highest-value unhandled container, at −28.8% to −58.2% on Silesia. Those
were *ideal* numbers — "compress the payload as if extracting it were free". It is
not free, and paying for it flips the sign.

## What was measured

LZ4 frames (`lz4.frame.compress`, level 1), parsed into their exact sequence list.
The packed form is what a real implementation must store:

```
plaintext | recipe | u32 recipe_len | u32 plain_len | "LZ41"
```

`recipe` holds the frame header verbatim, then per block its size, and per sequence
`varint(literal_len), varint(match_len), varint(offset)`. That is everything needed
to re-emit the original bytes exactly from the plaintext.

| fixture | raw | `.lz4` | sequences | recipe | engine on `.lz4` | engine on packed | result |
|---|---|---|---|---|---|---|---|
| spreadsheet XML | 3,250,684 | 980,883 | 292,391 | 1,056,411 | 121,343 | **181,709** | **+49.7% worse** |
| 4 MB of log lines | 4,021,362 | 968,383 | 236,281 | 979,978 | 119,019 | **179,641** | **+50.9% worse** |
| 1.65 MB of JSON rows | 1,650,000 | 6,884 | 52 | 274 | 289 | **209** | −27.7% |
| 400 KB of random bytes | 400,000 | 400,047 | 0 (stored) | 51 | 400,138 | 400,158 | −0.0% |

Same fixture, with the recipe zero-filled instead of written out — i.e. the
*ideal* case the earlier measurement described:

| fixture | engine on `.lz4` | ideal (free parse) | real |
|---|---|---|---|
| spreadsheet XML | 121,343 | 58,764 (−51.6%) | 181,709 (**+49.7%**) |

The ideal case was reproduced and then destroyed by the cost of storing the parse.

## Why it loses

The transform replaces "literals + LZ4's match descriptors" with "plaintext + our
match descriptors". A strong backend already codes LZ4's literals well and already
finds the redundancy the matched bytes represent — so the only thing that changes is
*whose descriptors are cheaper*. LZ4's own encoding is 1–2 bytes per sequence; ours
is three varints, and on this data they are barely compressible because they are
essentially random.

Trimming the recipe does not rescue it. Match lengths are derivable from the next
literal position, so only `(literal_len, offset)` is strictly needed — measured at
**72–76% of the full recipe**, still 744 KB against a 968 KB LZ4 file.

The pattern is exact: the transform pays only when LZ4 found *few, long* matches,
because that is when the descriptor stream is small. That is also when LZ4 already
did most of the work, which is why the win is small (27.7% on the JSON fixture,
which had 52 sequences for 1.65 MB) and never appears on data where LZ4 was
actually earning its speed.

## Why this differs from preflate

Preflate works because a DEFLATE parse is very nearly recoverable: the correction it
stores is a few bytes per stream, not per sequence. The earlier note claimed LZ4
would be easier than GIF because "the parse can simply be stored and re-emitted".
Storing is exact, but it is not cheap — and cheap was the whole question. Exactness
was never the risk here; the ratio was.

## What would change the answer

**Re-encode and compare.** Store only the compression parameters, run an LZ4 encoder
on the plaintext, and accept only when the output matches byte-for-byte. Then the
recipe costs nothing and the full ideal gain (−51.6% on the spreadsheet fixture) is
real. The catch is that it needs a bit-exact reproduction of liblz4's greedy matcher
— its hash table size, skip heuristic and last-five-literals rule — and a mismatch
means declining the file. That is the GIF problem again, with the difference that
LZ4's encoder is at least deterministic for a given version and level.

That is worth trying before this item is written off. What is **not** worth doing is
shipping the store-the-parse version behind a trial: it would fire only on the rare
file where LZ4 left few matches, and on everything else it would cost the trial and
return nothing.

## The re-encode route: tried, and it works (2026-11-18)

This document ended with "that is worth trying before this item is written off". It has been
tried, and the answer is yes on both questions.

**Are the parameters recoverable?** A frame is reproduced by trying the small grid of
(block size × level × block-linked) and keeping the one that matches byte-for-byte. Measured on a
2.5 MB XML fixture: 16 frames, made with four block sizes, two levels and both linkage settings,
and **16/16 were reproduced by searching a 16-combination grid**. LZ4's matcher is deterministic
for a given parameter set, so the search is exact rather than heuristic — a mismatch means decline,
which costs nothing.

The caveat, stated rather than buried: this was measured against **one liblz4 version** (the
python binding in this environment). liblz4's match finder has been stable for years, but "stable"
is not "proven across versions", and a frame written by a different major version would decline
until the grid includes it. Declining is the safe failure.

**What is it worth?** The engine on the `.lz4` against the engine on the plaintext it holds, which
is what a successful salvage replaces:

| file | bytes | engine `-9` |
|---|---|---|
| `data.xml.lz4` | 653,782 | **113,198** |
| `data.xml`, the plaintext inside it | 2,529,669 | **71,694** |

**36.7% smaller** — and the recipe costs a handful of bytes (the parameters), not the 1 MB of
sequence descriptors that killed the store-the-parse version. This is the ideal case the first
half of this document described, now reachable.

## Built, and the caveat was the whole story (2026-11-18)

`lz4_wrap.cpp` implements the route: parse the frame, decode it, search for the parameters that
reproduce it, store the plaintext, verify by rebuilding. `kLayer3Lz4` is layer 3 bit 4.

| frame | produced by | archive with | `--no-lz4` | worth |
|---|---|---|---|---|
| a text frame | **liblz4 1.10.1**, the version this build links | **71,707** | 107,254 | **+33.1%** |
| the same text | **liblz4 1.9.4**, a python binding | 113,198 | 113,198 | **0.0% — declines** |

**The version caveat turned out to be the whole story, not a footnote.** The earlier 16/16 result
was self-consistent — one library on both sides — and it over-generalised. LZ4's matcher changed
between 1.9.4 and 1.10.1, so a frame written by the older library cannot be reproduced by the
newer one, and the transform declines it. Declining is correct and costs a decode plus ten
re-encodes; firing is worth a third of the file.

That means the transform's value depends on **who produced the file**, and 1.9.4 is common. What
would widen it: try the older matcher too, which means linking a second liblz4 or porting the
1.9.4 match finder. Not done, and not obviously worth it — the frames most worth salvaging are the
ones this build can already reproduce.

**The header is read, not guessed.** Block size, block mode and the content-size flag are in the
frame descriptor, so only the compression level is searched: 13 levels instead of a 32-combination
grid. On a file that will decline anyway, that is the difference between 32 re-encodes and 10.

## The bit was allocated against a policy, and the policy was checked (2026-11-18)

`FORMAT.md` says a free `layers3` bit may be allocated only when four things hold. All four now do,
but the first was **not** met when the transform merged, and this section records both states.

| condition | status |
|---|---|
| 1. a measured win on **real** content, against the same archive with the transform disabled | **met — 2026-11-18.** The first measurement was a formula-generated XML fixture, which the policy says does not count. Re-measured on a **686,080-byte tar of this repository's own documentation**: 383,591 bytes as `.lz4`, archive **159,668 with the transform against 271,350 without — +41.2%**, byte-exact, `layers3 = 0x10`. |
| 2. a gate that declines, on a control | met — it declines a frame from liblz4 1.9.4, a real control rather than a self-made one |
| 3. a byte-exact inverse, verified on the same run | met — `lz4_unpack` rebuilds and compares before returning a payload |
| 4. a test that asserts the transform fired | met — `integration.corpus` case 8 checks the layer bit and that the archive shrank |

**The real-content number is larger than the synthetic one** (+41.2% against +33.1%), which is the
opposite of the BMP lesson the policy was written from — but that is luck, not vindication: a tar of
markdown is close to the best case for this transform, and it is still one file.

**What remains unmeasured is the hit rate**, not the payoff: how often a `.lz4` in the wild was
written by the liblz4 this build links. That is the open question, and it is a sampling question
about other people's files rather than anything a fixture can answer.

## The version question, answered from the source (2026-11-18)

The open decision was "widen backwards to liblz4 1.9.4, or not". Checking upstream showed the
question was the wrong shape: **liblz4 1.11.0 was released 2026-11-04**, so the fleet's 1.10.1 is
already one generation behind, and there are at least three matcher generations in the wild.

**Diffing 1.10.1 against 1.11.0** (`lib/lz4.c`, 2,922 against 2,953 lines, 36 diff lines) gives a
precise answer:

| diff | meaning |
|---|---|
| 28 lines | obsolete `LZ4_compress*` declarations **re-added** for ABI compatibility — no behaviour |
| **4 lines** | **one behavioural change**, in the match-finder |

```c
-  {   U32 const ctxIndex = prevIndex + 1;
+  {   int const usedTable = (dictDirective == noDict) && (dictIssue == dictSmall);
+      U32 const ctxIndex = ((usedTable && (prevIndex < startIndex)) ? startIndex : prevIndex) + 1;
```

`ctxIndex` is the table slot consulted for the next match. It now clamps a stale entry to
`startIndex` when the table is being reused. `dictSmall` is reached from exactly one place
(`LZ4_compress_generic_continue`): **`srcSize < LZ4_64Klimit` (64 KiB) *and* `ctx->currentOffset != 0`** — a
small block compressed through a **re-used streaming context**.

**So the change is narrow, and it is knowable:**

- one-shot compression of a fresh context → **unchanged** (`noDictIssue`, not `dictSmall`)
- any input **≥ 64 KiB** → **unchanged**
- a **sub-64 KiB block through a re-used streaming context** → **may differ**

That is why the re-encode-and-compare design is the right one here: the version boundary is not a
wall between "all 1.10 frames" and "all 1.11 frames", it is **per frame**. A 1.11.0 frame that never
touches the small-block path is reproduced byte-for-byte by the 1.10.1 matcher and salvaged normally.
Only the narrow case declines, and it declines safely — the file is stored unchanged.

**Recommendation, now that it is measurable:** do not widen backwards. Bump the vendored liblz4 to
1.11.0 and re-run the corpus, then record how many frames changed. If none do, the bump is free and
the transform covers both generations. If some do, the count says whether the old generation is worth
a second matcher — and that is the sampling question, answered by files rather than by argument.

### Measured: 1.10.1 and 1.11.0 produce identical frames on everything tried

The source diff above says the change *can* matter. Compiling both libraries and compressing the
same inputs settles whether it *does* on our data. Both versions were built from their own `lib/`
trees and driven through the same probe — one-shot `LZ4F_compressFrame` (4 MB blocks, linked) and a
**streaming** pass with **16 KiB chunks**, which is the path that reaches `dictSmall` (small block,
re-used context).

| input | bytes | one-shot 1.10.1 | one-shot 1.11.0 | streaming 1.10.1 | streaming 1.11.0 |
|---|---|---|---|---|---|
| ui.png | 92,822 | 88,852 | **88,852** | 88,930 | **88,930** |
| photo.pdf | 24,387 | 23,534 | **23,534** | 23,674 | **23,674** |
| alice.txt | 151,884 | 84,928 | **84,928** | 84,945 | **84,945** |
| periodic4k.bin | 32,768 | 169 | **169** | 161 | **161** |
| shift.bin | 41,197 | 1,979 | **1,979** | 1,971 | **1,971** |
| zeros_then_text.bin | 80,000 | 371 | **371** | 376 | **376** |
| rand_then_repeat.bin | 185,600 | 1,024 | **1,024** | 1,042 | **1,042** |

Byte-identical in **14 of 14** cells — same size and same FNV-1a hash of the frame. The last four
inputs were built specifically to create the stale table entries the change is about (periodic
motifs, shifting runs, zeros then text, random then repetitive).

**What this does and does not prove.** It shows the change is unreachable from these inputs, not that
it is unreachable from all inputs: `prevIndex < startIndex` needs a stale slot that also survives the
`dictSmall` range check further down, and when it does not, the candidate is rejected either way.
So the honest position is: **the bump is free on everything measured, and the residual risk is a
narrow corner no fixture here reached.**

That is enough to recommend the bump, and not enough to claim the versions are equivalent. If the
vendored library moves to 1.11.0, the corpus gate is what proves it — the transform verifies every
frame it rewrites, so a divergence would decline rather than corrupt.

## Bumped to 1.11.1 (2026-11-19) — and the frames did not change

The recommendation above was "bump to 1.11.0 and re-run the corpus, then record how many frames
changed". Done, one version later than recommended, for a reason worth stating.

**1.11.1, not 1.11.0.** Upstream shipped 1.11.1 on 2026-11-10, six days after 1.11.0. Diffing the
two `lib/` trees shows what changed: ppc64 fast-decode paths, CLI fixes, and one library fix —
`LZ4_resetStreamHC()` could write out of bounds at levels ≥ 10 on a state containing garbage (since
1.11.0). **The match finder is untouched**, so the 14-of-14 measurement taken against 1.11.0 carries
over unchanged, and the point fix is the version a decompressor should link.

The 1.10.1 → 1.11.0 behavioural line recorded earlier is present exactly as described:

```c
-            {   U32 const ctxIndex = prevIndex + 1;
+                U32 const ctxIndex = ((usedTable && (prevIndex < startIndex)) ? startIndex : prevIndex) + 1;
```

### How many frames changed: zero, on ten frames

The vendored encoder was built at both versions and driven through the same probe — one-shot
`LZ4F_compressFrame` (4 MB blocks, linked) and a streaming pass with 16 KiB chunks, which is the
path that reaches `dictSmall`. Five inputs: a tar of this repository's documentation, plus four
built to create the stale table entries the change is about (periodic motifs, shifting runs, zeros
then text, random then repetitive). Sizes and FNV-1a hashes of the frames:

| input | one-shot 1.10.1 | one-shot 1.11.1 | streaming 1.10.1 | streaming 1.11.1 |
|---|---|---|---|---|
| `docs.tar` 880,640 | 524,452 | **524,452** | 524,452 | **524,452** |
| `periodic4k.bin` 4,096 | 52 | **52** | 52 | **52** |
| `rand_then_repeat.bin` 194,600 | 94,314 | **94,314** | 94,314 | **94,314** |
| `shift.bin` 41,197 | 1,615 | **1,615** | 1,601 | **1,601** |
| `zeros_then_text.bin` 80,000 | 371 | **371** | 371 | **371** |

**10 of 10 identical, size and hash.** Combined with the 14 of 14 measured against 1.11.0, that is
24 cells with no divergence — and the residual risk is unchanged and narrow: a stale slot that also
survives the `dictSmall` range check.

### The transform still fires, and pays

A frame produced by the new encoder, compressed by the new build:

| | archive |
|---|---:|
| `docs.tar` as `.lz4` (524,452 bytes), transform on | **249,207** |
| the same file, `--no-lz4` | 406,454 |

**38.7% smaller**, `layers3 = 0x10`, byte-exact. Bigger than the 41.2% recorded earlier because the
fixture is larger (880,640 against 686,080 bytes of the same documentation).

### Gates

- `ctest`: **38/38 suites**, including `unit.lz4` and `integration.corpus` case 8 (the layer bit and
  the size drop).
- `scripts/holdout.py --check`: 4 files, **+0 bytes**, hashes match.

### One thing this does not fix

`vendor/hcs/SOURCES` still records lz4 as "1.10.1", and that row lives in the **HCS** repository —
the vault, not this one. Until HCS updates it, `check-vendor-current.sh` compares a SOURCES file that
is still accurate to *HCS main* while the tree it describes has moved. The bump is pinned to a real
tag this time (`v1.11.1`), which removes the "states a version with no tag of that name" caveat that
kept lz4 in the `copy` column.
