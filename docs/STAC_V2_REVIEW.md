# STAC-v2 plan reviewed against this engine's measurements

The STAC-v2 plan proposes a six-stage pipeline:
`Type Oracle → Precomp → DST → LZP → Parse → Entropy`.

This reviews it stage by stage against what ziptastic has **actually measured**.
Where the plan and the measurements disagree, the numbers are given; where a claim
has not been tested here, it says so rather than guessing.

## Headline

**The plan's central thesis is right, and we have already proved it.** It says the
ratio advantage comes mostly from **Precomp**, not from brute-force CM. That is
exactly what this engine found: precompression gave −19.4% (JPEG), −29.5% (GIF),
−79% (PDF), −28.8% (ZIP), while every entropy-level idea measured 1–3% or lost
outright.

**But the plan badly under-estimates Precomp and over-estimates several other
stages.** Its "8–18% gains" for precompression is far too conservative — measured
here, a single PDF recompression was −79%.

**And its architecture is a different family.** STAC-v2 is a CM (lpaq-style)
design; ziptastic is BWT plus per-block backend selection plus precompression.
They agree on the most important thing (transforms before entropy coding) and
diverge on everything after it.

---

## Stage by stage

### Stage 0 — Type Oracle (1 MB fingerprint → TEXT/STRUCTURED/NUMERIC/EXEC/CONTAINER)

**Not worth it for ratio, already superseded here.** This engine tried a
sample-based selector **twice** and rejected it both times on measurement: −1.8%
time (inside noise) for **+2.68% size**. The reason is structural — trying every
backend and keeping the smallest is *strictly better for ratio* than guessing one
from a fingerprint; the oracle only wins on speed.

Its real value in STAC-v2 is gating which *transforms* run, which is a speed
optimisation, not a ratio one. Fair, but it is not the "~0 cost" claim — a wrong
classification costs ratio, and the fingerprint is the same idea we already
measured failing.

### Stage 1 — Precomp (the plan's "biggest ratio win")

**Already built, and bigger than the plan estimates.** Shipped layers:

| container | measured gain |
|---|---|
| PDF streams | **−79.1%** |
| GIF (LZW → indices) | **−29.5%** |
| JPEG (brunsli) | **−19.4%** |
| ZIP/DEFLATE (preflate) | −28.8% vs `xz -9` |
| LZ4 | **−28.8% to −58.2%** (measured, integration pending) |

The plan lists "deflate/gzip/zlib/brotli/zstd". We have DEFLATE and LZ4 measured;
ZSTD is the obvious next one. Its note that "the work is in porting, not
inventing" is exactly right, and it is what we have been doing.

**One correction:** the plan says precomp is "fragile — bit-exact recompression
requires careful handling". True, and this engine has been bitten three times
(PDF's stray EOL byte, PDF's zlib wrapper, GIF's sub-block framing). The
mitigation that works is **verify-before-use**: never commit a transform whose
round trip has not been checked on that exact input. That single rule turned each
of those bugs into a declined transform instead of a corrupt archive.

### Stage 2 — DST (per-family transforms)

**NUMERIC — "bitshuffle → transpose → predictive residual, beats delta by 6–12%".**

Partly contradicted, partly untested, and the distinction matters:

- **bitshuffle alone: measured losing.** +4.8% to +27.6% worse — *including on the
  16-bit image data it was designed for*.
- **delta alone: measured losing.** +1.7% to +91% worse.
- **But the plan's version is different** — per-column, with a *predictor chosen per
  column and stored*. That is typed decomposition, not blind reordering, and it is
  the one numeric idea we have **not** tested. OpenZL's parsers point the same way.

So: do not implement "bitshuffle"; **do** test per-column predictive residuals with
a stored per-column predictor. That is a real experiment, not a re-run of a failed one.

**STRUCTURED — Re-Pair grammar induction on tag skeletons, "4–9% on SVG/G-code/XML".**
Untested here. Plausible: it is a genuine grammar compressor and our BWT is not.
Worth an experiment, gated by size as the plan itself suggests (it will not pay on
small files).

**TEXT — word-piece tokenizer + MTF + RLE, "3–6%".**
**Measured much smaller.** A word-dictionary stage here gave **−0.38% on `dickens`
and +0.93% on `reymont`** — it helps English and hurts Polish. The plan's 3–6% is
optimistic for this data; and it needs the selector to gate it, which we already
have.

### Stage 3 — LZP ("the single best free ratio trick, 5–12%")

**Measured and it did not pay.** LZP inside libbsc gave **−0.02% to +1.9%** across
the corpus — a wash to slightly negative.

The honest caveat in the plan's favour: ours was LZP **before BWT**; theirs is LZP
**before CM**. Different placement, and a CM coder may benefit where a BWT coder
does not, because BWT+QLFC already captures short repeats. So the claim is not
refuted for their architecture — but it is refuted for ours, and it is the one
place the plan's confidence is least supported by evidence we have.

### Stage 4 — Optimal parse (2–4%)

**Already have it.** Our parser does optimal parsing; the gain is already in the
corpus numbers. Applying it *after* LZP is a different composition and untested.

### Stage 5 — Tiered CM (StateMap + 7–8 models + 2-layer mixer + 2 APM/SSE + 2 interleaved range coders)

This is the most valuable technical content in the plan, and it matches catalogue
item #1 in `ALGORITHMS_FROM_PAPERS.md`. Specific notes:

- **StateMap bit-history — take it.** We have nothing like it. It is cheap, standard
  (lpaq/paq), and it is the difference between a toy context model and a real one.
  My own BWT+CM attempt lost 4.5–11.3% precisely because it had a single order-1
  context and no state machine.
- **7–8 models rather than 12 — right instinct.** The plan's "most of the ratio,
  40% less compute" is consistent with the PAQ literature.
- **2 interleaved range coders for "25–35%" — measured at ~12% here.** Our
  interleaved rANS with 4 lanes gave **~12%** for 9 bytes of payload. The plan's
  figure is optimistic; expect roughly a third of it.
- **Model pruning by mixer weight — good, take it**, but it only exists once there
  is a mixer to prune.
- **Nibble CM for fast profiles — reasonable**, and a standard trade.

### Speed profiles (Fast 12/35 MB/s … Archive 0.2/2 MB/s)

These are honest and sensibly derived, and the plan is right that a naive
implementation lands far below them. Two things from our side:

- Our decode target was 150 MB/s and is **not met**; the entropy coders are the
  bottleneck, which is why FSE/tANS (catalogue #2) is the speed lever we identified
  independently.
- **Block-parallel decode with a shared static dictionary trained at encode** is a
  genuinely good idea we have not tried. Our parallel mode costs **+3.4% size for
  −29% time**; a shared dictionary is exactly the mechanism that would reduce that
  size penalty.

---

## What I would take from this plan, ranked

1. **StateMap bit-history** — the cheapest real upgrade to any CM work we do.
2. **Per-column predictive residual with a stored per-column predictor** — the one
   numeric idea we have not tested, and our failed bitshuffle result does not apply
   to it.
3. **Shared static dictionary for parallel blocks** — reduces a measured 3.4% size
   penalty.
4. **Re-Pair on structured data** — untested, plausible, gated by size.
5. **Model pruning** — practical, once a mixer exists.
6. **Nibble CM for fast profiles** — standard trade, worth having.

## What I would not take, on the evidence

- **Type Oracle for ratio** — measured negative; try-everything wins for ratio.
- **LZP** — measured at −0.02% to +1.9% in our architecture.
- **bitshuffle/delta as such** — measured losing, repeatedly.
- **Word tokenizer at 3–6%** — measured at −0.4% to +0.9%.
- **Interleaved coders at 25–35%** — measured at ~12%.

## The strategic difference, stated plainly

STAC-v2 is a **CM-first** design: transforms to make CM's job easier, then a big
CM. ziptastic is a **container-first** design: parse the container, then use a
per-block selector over several entropy backends.

Both agree on the most important finding either project has produced: **spend
compute on transforms, not on brute-force modelling.** The evidence here says the
container side is where the big numbers are (−19% to −79%), and the modelling side
is worth single digits. If STAC-v2 is built, it should be built precomp-first, and
the plan does say that — its own stage ordering is right even where its per-stage
estimates are optimistic.
