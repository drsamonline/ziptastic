# Algorithms worth implementing from published descriptions

The rule this document works under: **a published paper, RFC or format
specification is a legitimate source. Source code is not.** Everything below is
described in public literature and can be implemented cleanly, which is the route
the coarse index and dedup stage already took from SREP's published technique.

Every gain below is an **estimate with stated reasoning**, not a measurement.
Where something has been measured, it says so.

---

## 1. Logistic mixing + SSE for the BWT+CM stage — the big one

**Published in:** Mahoney, *Data Compression Explained* (free), and the PAQ papers.

**Why it matters here:** my own BWT+MTF+RLE+CM attempt lost to libbsc's QLFC by
4.5–11.3% (measured, `GEMS_REPORT.md`). The reason is not the pipeline — it is the
model: I used a *single* order-1 context. PAQ-family compressors do three things I
did not:

- **many context orders at once** (order 0–6 over the MTF stream, plus a
  word/record context), each a separate predictor;
- **logistic mixing** — the predictors' outputs are combined by a learned mixer in
  the logistic domain, rather than picking one;
- **SSE/APM** — a secondary symbol estimator that refines the mixed probability
  against the actual bit history.

bzip3 beats libbsc by exactly this margin, so the target is known to exist.

**Estimated gain:** takes BWT+CM from ~5% *behind* QLFC to plausibly 5–10% *ahead*
on binary and text. On the corpus that would be several percent overall.
**Effort:** high — this is a modelling project, weeks not days.
**Risk:** medium; the pieces are well documented but tuning is empirical.

## 2. FSE / tANS as an entropy backend — the speed one

**Published in:** Collet, *Finite State Entropy* (zstd's documentation describes the
format; RFC 8878 documents the resulting stream).

**Why it matters here:** the 150 MB/s decode target is unmet, and the reason is the
entropy coders. rANS decodes one symbol at a time with a division-free but
still serial state update. tANS is **table-driven**: decode is a table lookup plus
a bounded shift, with no renormalisation branch in the common case. It is the
standard answer to exactly this problem.

**Estimated gain:** decode 2–3× faster at essentially identical ratio (both are
ANS variants; the difference is table layout, not modelling).
**Effort:** medium — a contained new backend behind the existing mode byte.
**Risk:** low. It cannot lose ratio, only fail to be faster.

## 3. Repeat-offset codes (rep0/rep1/rep2) in the token model — the cheap ratio win

**Published in:** the LZMA format description, and RFC 8878 §3.1.5 (zstd's
"repeat offsets"). This is also what ARC's REP preprocessor exploits.

**Why it matters here:** our token model codes every match distance through a
position model. In real data distances **repeat** heavily — the same phrase recurs
at the same distance many times. Coding "same distance as last time" as a single
bit, and two more recent distances as short codes, removes most of that cost.

**Estimated gain:** 1–3% on text and structured binary. This is the REP gain, but
inside the parser where it is chosen optimally rather than as a preprocessing pass
— which is why it should beat a REP stage rather than duplicate it.
**Effort:** medium — token format, both coders, the price model.
**Risk:** low-medium; it is a well-trodden design.

## 4. bzip2's exact MTF/RLE scheme — the cheap version of #1

**Published in:** Seward's bzip2 documentation and thesis.

**Why it matters here:** bzip2 uses RUNA/RUNB for zero runs (which I did) but then
codes the result with **2–6 Huffman tables selected by context**, switching tables
every 50 symbols. That is a cheap approximation of context modelling, and it is
much less work than full mixing.

**Estimated gain:** recovers part of the gap in #1 — plausibly half of it.
**Effort:** low-medium.
**Risk:** low.

## 5. LZMA-style literal context — small but nearly free

**Published in:** the LZMA format description.

**Why it matters here:** our literals are coded with an order-1 context (previous
byte). LZMA additionally conditions on **the byte at the match position** — the
"match byte" — which is strong evidence when a match just failed late. It is a few
lines in the literal model.

**Estimated gain:** 1–2% on text.
**Effort:** low.
**Risk:** low.

## 6. Brotli's static dictionary — narrow but free

**Published in:** RFC 7932 (the dictionary itself is part of the spec).

**Why it matters here:** a built-in table of common web strings lets tiny HTML/JS
files start with matches rather than literals. Irrelevant to large files, useful
for the many small files in a real archive.
**Estimated gain:** meaningful on web assets, ~0 elsewhere.
**Effort:** low.
**Risk:** low.

---

## Already covered, do not redo

- **PPMd with SEE** — our vendored `Ppmd7` already implements secondary escape
  estimation (the `See`/`BinSumm` tables in `Ppmd7.c`). Checked, not assumed.
- **Optimal parsing** — our parser does this; Tornado's approach is architecturally
  the same and offers little.
- **Long-range matching** — the coarse index (Phase 6) and dedup stage (Phase 13)
  already cover SREP's technique.

## Measured and closed — do not revisit

| idea | measured result | verdict |
|---|---|---|
| delta preprocessing (1/2/4-byte) | +1.7% to +91% worse | dead |
| byte-shuffle (bitshuffle's core idea) | +4.8% to +27.6% worse, on the data it targets | dead |
| LZP inside libbsc | -0.02% to +1.9% | dead |
| sample-based backend selector | -1.8% time (noise) for +2.68% size | dead |
| naive BWT+MTF+RLE+CM | +4.5% to +11.3% worse than QLFC | dead *until* #1 |

The pattern behind the first two: **BWT+QLFC already models byte statistics**, so
any transform that reorders bytes destroys the context the coder depends on. That
result generalises — bit-plane splits and wavelet transforms should be expected to
fail for the same reason.

---

## Recommended order

1. **#3 repeat-offset codes** — best ratio gain per unit of effort, well documented,
   and it exploits a redundancy our token model currently pays full price for.
2. **#2 FSE/tANS** — the only realistic path to the decode target, and it cannot
   cost ratio.
3. **#5 literal match-byte context** — nearly free, do it alongside #3.
4. **#1 full mixing** — the largest prize, but a modelling project; only worth
   starting if someone wants to own it.
5. **#4, #6** — cheap, narrow; take them when convenient.

## On "enhancing beyond the original"

Where our position is genuinely better than the published baseline, and worth
pushing rather than merely copying:

- **We choose per block.** bzip3 and libbsc apply one backend to everything; we
  already take the smallest of many per block. Any new backend benefits from that
  immediately, which is why bzip3-as-plugin gained on `xml` even though it is 6.6%
  worse on that file overall.
- **We have precompression.** No general-purpose compressor in this literature
  handles ZIP/PDF/JPEG/GIF before coding. That is where our large wins came from
  (-19.4% JPEG, -29.5% GIF, -79% PDF) and no amount of entropy-model work competes
  with it.
- **The honest ceiling:** on ordinary text and binary we are already 11.8% below
  `xz -9`; the algorithms above are worth single-digit percentages on top. The
  remaining large wins are in *formats we do not yet handle* (MP3, and the rest of
  precomp's list), not in better modelling.

---

## Attempt log

### #5 literal match-byte context — attempted, reverted, with diagnosis

Implemented LZMA's idea: 32 literal contexts instead of 16, the extra 16 indexed
by the **match byte** and used only for the first literal after a match. Encoder
(`build_symbols`), the range encoder and all three decoders were updated
consistently. It did not land, and the reasons are worth recording because they
are the traps in this file:

1. **Table ids were hard-coded.** `kLen0 = 20`, `kDist0 = 24`, `kBypass = 28` were
   literals, so widening `kLitCtx` to 32 made literal ids 4..35 run straight
   through the length and distance tables. **Fixed and landed** — the ids are now
   derived from the context counts, which makes that class of bug impossible.
   The values are unchanged today.
2. **The match context must persist across chunks.** The chunked rANS path calls
   `decode_chunk` per chunk; declaring `last_dist` inside it resets the state at
   every chunk boundary while the encoder walks the whole block with one running
   value. Hoisting it out of the lambda is required.
3. **`recorded` must use the same context as the coder.** The decoder records each
   symbol so the adaptive model can advance; recording `prev >> 4` while coding
   with the match-byte context desynchronises the two models.

With 1–3 fixed, `unit.roundtrip` still aborted with `std::bad_alloc` in
`parse_path_roundtrips`, so at least one more inconsistency remains — most likely
in the adaptive (chunked) encoder path, which builds its symbol stream separately
from the static one. Rather than leave the tree red for a 1–2% feature, the change
was reverted; only the table-id fix remains.

**For whoever picks this up:** the failure is a desync, not a logic error, so the
fastest route is to assert that the encoder's and decoder's context choices match
symbol-by-symbol for a small input, rather than reading the code.
