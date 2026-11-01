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

---

# Implementation attempt: wired, but the gate blocks it

Recursion is implemented — a `kLayerNumericNested` layer bit, the numeric transform
re-entered on what precomp inflated, and undone *before* precomp on decode because
that is the order it was applied. It round-trips exactly.

**But it does not fire on the case it was built for**, and the reason is a genuine
bug worth recording.

## The numbers that show it should fire

| what | our engine's output |
|---|---|
| the XLSX as given (573,983 B) | 369,080 |
| the sheet XML inside it (3,262,191 B) | **181,633** |

Reaching the XML is worth **2×**. `preflate_unpack` succeeds and produces exactly
that XML. And yet `precomp_helps()` declines.

## Why

`precomp_helps()` estimates the cost of both sides by running **one** code path —
`StaticCostModel` + `parse_block` + `encode_tokens_range`, i.e. the range-coder token
path — and comparing the two estimates.

The full engine, on the same XML, picks **BWT** and gets 181,633. The range-coder
path is much weaker on XML, so its estimate for the payload lands near or above its
estimate for the deflated original, and the comparison fails.

**The gate is measuring a different backend from the one that will actually be used.**
That is the same class of error as the sample-based selector (+2.68% size) and the
256 KiB mid-sample that once made a 38 MiB text payload look 60% less compressible
than it was. Three instances now, all the same shape: a cheap predictor standing in
for a measurement, and getting it wrong in the direction that costs ratio.

## The fix, and why it is not a one-liner

The honest fix is to compare the way the engine will actually decide: best-of-backends
on both sides. That is expensive at file level, which is why the predictor exists.

Two options:

1. **Add a BWT term to `coded()`** — sample both sides, run libbsc as well, take the
   smaller. Costs one extra BWT pass per decision on the sample only.
2. **Drop the gate for verified transforms.** `preflate_unpack` only succeeds on real
   DEFLATE, and the round trip is verified before use. The gate exists to avoid
   pathological cases, but it has now been wrong three times, and every one of those
   errors cost ratio rather than correctness.

Option 1 is the smaller change and keeps the gate doing its job. It is the next
commit.

## What is already in the tree

- `kLayerNumericNested` (bit 1 of `layers`), with `kLayerMask` widened.
- Encode: after precomp inflates, `numeric_pack` is re-entered on the payload, and
  the result is accepted only if `numeric_unpack` reproduces the payload exactly.
- Decode: nested numeric undone **before** `preflate_repack`.
- `precomp_helps` gained an exact path below 8 MiB that codes both sides in full
  instead of extrapolating from a 256 KiB sample. It does not fix this case, because
  the problem is *which* code path it uses, not how much of the input it sees.

---

# Fixed: the gate was 2x pessimistic, and a structural signal beats it

Instrumented the decision and got the actual numbers:

```
precomp_helps: payload 3,262,505 -> 390,766 | original 573,983 -> 378,911 => DECLINE
```

The estimator scored the inflated XML at **390,766**. The real engine scores the same
bytes at **181,633**. A **2.1x pessimism**, and it is the whole reason the transform
was declined. Adding a BWT term to the estimator was not enough — the estimate stayed
wrong.

So the fix is not a better predictor. It is **a structural signal instead of a
prediction**:

> If inflating expanded the data by 2x or more, the original was DEFLATE over
> something highly compressible. Take the payload.

That is a fact about the input, not a guess about the output, and it needs no
estimator. The estimator is still used for the ordinary case (modest expansion),
where it has been adequate.

## Result

| file | before | after | gain |
|---|---|---|---|
| `book.xlsx` (573,983 B) | 369,080 | **181,791** | **−50.7%** |

`flags = 0x05` — precomp engaged. Round-trip exact. 21/21 suites pass.

The nested numeric transform did **not** fire (`layers = 0x00`), and correctly: the
payload is XML — `<row r="1"><c r="A1"><v>1003</v></c>` — not delimited text, so the
CSV transform has nothing to bite on. Reaching −89.4% on this shape needs an
XML-aware numeric transform that recognises cell values, which is a separate item.

## What this cost and what it taught

Three separate predictors have now been wrong in this project, all in the direction
that costs ratio:

1. the sample-based backend selector — +2.68% size
2. the 256 KiB mid-sample that made 38 MiB of text look 60% less compressible
3. this one — 2.1x pessimism, blocking a −50.7% transform

The pattern is consistent enough to be a rule: **where a structural fact about the
input is available, use it; reserve prediction for cases where no such fact exists.**
