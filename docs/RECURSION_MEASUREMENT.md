# Recursive container descent: measured, and it is the biggest result so far

Two models led with this idea and one put it above every new format. Measured it
before building it, the same way as LZ4 and base64.

## The experiment

A realistic nested artefact: a **`.tar.gz` containing an `.xlsx`**, where the xlsx is
a ZIP whose `sheet1.xml` holds two columns of 40,000 integers — which is what a
spreadsheet, a backup, or a downloaded report actually looks like. Then measure what
each level of reach buys.

| step | size | vs baseline |
|---|---|---|
| nested `.tar.gz` (what a backup contains) | 369,296 | — |
| **recursion**: reach the sheet XML inside | **181,668** | **−50.8%** |
| **recursion + our existing numeric transform** | **38,984** | **−89.4%** |

## What this says

**Recursion alone halves the file.** No new format, no new coder, no new model — just
re-running detection on the output of a transform that already exists. The `.tar.gz`
and the `.xlsx` are both containers we already parse; the chain simply stopped after
one level.

**Recursion multiplies the transforms we already have.** The numeric transform alone
was worth −77.4% on a bare CSV. Through two container layers, the same code is worth
**−89.4%** — because it is now reaching data it could never see. That is the
multiplier argument, and it is much larger than it looked on paper:

- numeric alone on CSV: −77.4%
- numeric with recursion through tar.gz → xlsx: −89.4% on the whole nested file

**This is the largest single measured result in the project**, above PDF (−79.1%) and
above the bare numeric transform (−77.4%). And it is the cheapest: the transforms
already exist, and the change is to re-enter detection on transform output with a
depth limit.

## Why the models were right

Both the models that proposed this argued it from the same place: *real archives nest
two or three deep, and every transform we own is single-pass.* The measurement
confirms it and puts a number on the multiplier. This is the one suggestion across
twelve reviews that makes **existing** work pay more rather than adding new work.

## The design that follows

1. After any Layer 0 transform produces output, **re-run detection on the result**.
2. **Depth limit** — 3 is enough for tar → xlsx → xml; deeper risks pathological
   cases and unbounded work.
3. **Size floor** — do not recurse into payloads below some threshold; the header
   cost is real (we already learned this with the numeric column gate).
4. **Cycle guard** — a transform that produces its own input must not loop. Track a
   hash of each level.
5. The existing **verify-before-use** applies at every level, and the outermost
   verification still covers the whole stack, so a bug at depth 2 is caught by the
   same check that guards depth 1.

## Ordering, unchanged

Recursion stays first, and this is why: it is the only item that makes the other
eight more effective. Base64 was measured at 4.5% on dense data and 71.8% on
structured data — and recursion is what *finds* the structured data. The two compound
in exactly the way the experiment shows.
