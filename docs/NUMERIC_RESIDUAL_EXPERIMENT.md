# Typed numeric residual: measured

STAC-v2 proposed a per-column transform — detect numeric columns, trial delta /
linear / quadratic predictors, store the winner per column. It was the one idea
neither project had data on, and the plan predicted **+5–15% on smooth scientific
columns**.

Measured here. **The prediction is wrong in both directions**, and the corrected
version is much stronger than the plan hoped.

All figures are `xz -9` on the encoded column (a proxy for our engine's entropy
stage), 40,000 rows, synthetic but realistic generators.

## Integers: large, consistent wins

| column | raw | delta | linear | quadratic | best |
|---|---|---|---|---|---|
| counter (small steps) | 18,588 | **10,080** | 12,220 | 14,132 | delta **−45.8%** |
| monotonic (random steps) | 41,380 | **32,908** | 36,424 | 41,988 | delta **−20.5%** |
| sensor (noisy sinusoid) | 46,648 | 46,104 | 48,476 | 51,644 | delta −1.2% |
| random | **162,020** | 172,476 | 172,304 | 176,572 | **raw** — trial declines |

Two things to note. Higher-order predictors (linear, quadratic) **lose to plain
delta** everywhere here: once you have taken a first difference of an integer
column, the rest is noise, and a second difference amplifies it. And the trial
**correctly declines** on the random column — the safety property works.

## Floats: mostly not worth it

| column | raw | xor (8-byte) | gorilla (leading-zero trimmed) | best |
|---|---|---|---|---|
| smooth sinusoid | 248,572 | **247,164** | 258,500 | xor **−0.6%** |
| noisy | **302,460** | 305,684 | 318,584 | raw |
| coordinates (near-constant) | 204,352 | **164,096** | 173,464 | xor **−19.7%** |
| temperature (smooth + tiny noise) | 244,780 | **243,264** | 256,924 | xor −0.6% |

**This is the important negative result.** The plan's target case — "smooth
scientific data" and "decimal-XOR floats" — gives **−0.6%**, i.e. nothing. Floats
only pay when they are *nearly constant* (coordinates: −19.7%), which is a narrow
case, not a general one.

## Gorilla's leading-zero trick loses to plain XOR here

The standard float-compression technique (XOR with the previous value, then emit
only the significant bytes) is **worse than emitting the full 8-byte XOR** when the
backend is a strong general compressor:

- smooth: gorilla 258,500 vs xor 247,164
- coordinates: gorilla 173,464 vs xor 164,096

The reason is that Gorilla's length bytes **break the byte alignment** that a
BWT/LZMA-class match finder depends on, while the zero bytes it removes are already
almost free. Gorilla was designed for streaming systems with tiny state (timestamps
in a time-series database), not for a compressor with a large model. Adopting it
because it is "the standard technique" would have made things worse.

## The decisive test: real CSVs are text

CSVs in the wild hold decimal text, not binary. Does converting to binary and taking
residuals beat the text that our engine already compresses well?

| column | text | binary | binary + delta |
|---|---|---|---|
| counter | 20,960 | 18,548 | **10,100** (−51.8% vs text) |
| monotonic | 45,376 | 41,352 | **31,372** (−30.9% vs text) |

And on a four-column integer CSV, per-column binary+delta against the text file:

| | size |
|---|---|
| whole CSV as text | 163,616 |
| per-column binary + delta | **42,088** |
| gain | **−74.3%** |

That is the same order as the container wins (PDF −79%, GIF −29.5%), not the
single-digit order the plan predicted.

## Corrected conclusion

**The transform is worth building, but it is an *integer column* transform, not a
"scientific float" transform.** Concretely:

1. **Target integer columns.** −20% to −52% per column, −74% on a whole CSV.
2. **Drop the float path** unless the column is nearly constant (a narrow special
   case). Plain 8-byte XOR if anything; **not** Gorilla.
3. **Only delta.** Linear and quadratic predictors lost in every column tested.
   That also removes the need to store a predictor ID — one predictor, no trial
   overhead beyond accept/reject.
4. **Keep the per-column accept/reject trial.** It correctly declined on random and
   noisy data, which is what makes the transform safe to apply automatically.

## The hard part, which the plan does not mention

For a **text** CSV, converting `"12345"` to binary and back must reproduce the exact
characters. That fails on `"007"`, `"+5"`, `"5.0"`, `" 5"`, `"1,234"`, `"5e3"` and
any column with mixed formatting. Two options:

- **Decline non-canonical columns.** Cheap, safe, and loses only the messy ones.
- **Store a per-column format descriptor** (width, leading zeros, sign policy).
  More work, catches more.

Start with declining. It is the same discipline as verify-before-use: transform
only what provably round-trips, and measure the rest later.

## Where this leaves the STAC-v2 test order

Their suggested order was: CM backend first, numeric residual second. **The data
says reverse it.** The numeric residual has a measured −74% on a whole file; the CM
backend has an expected +5–10% on text. The residual is also far cheaper to build —
it is a Layer 0 transform of exactly the kind already shipped three times, with no
new entropy coder.

---

# Follow-up: residual encoding, size gate, block boundaries

Three questions raised after the first experiment, answered with measurement.

## 1. Residual encoding — zigzag+varint wins, Rice loses

Deltas of an integer column are small and signed. How they are *encoded* before the
backend sees them changes the result:

| encoding | counter (delta 1–1003) | monotonic (delta 0–309122) |
|---|---|---|
| raw 8-byte delta | 10,100 | 31,372 |
| **zigzag + varint** | **9,260 (−8.3%)** | **29,792 (−5.0%)** |
| Rice, k=2 | 11,344 (+12.3%) | 36,288 (+15.7%) |
| Rice, k=3 | 9,356 (−7.4%) | 37,844 (+20.6%) |
| Rice, k=4 | 11,268 (+11.6%) | 37,104 (+18.3%) |
| Rice, k=6 | 11,576 (+14.6%) | 34,728 (+10.7%) |

**Zigzag + varint wins on both columns. Rice loses on both.** The format decision is
therefore: zigzag the delta, varint it, and hand the bytes to the normal pipeline.

## 2. The pattern behind Gorilla and Rice

Both negatives are the same finding:

- **Gorilla** (leading-zero-trimmed float XOR): lost to plain 8-byte XOR.
- **Rice** (bit-packed quotients and remainders): lost to byte-aligned varint.

Both are **bit-level encodings that break byte alignment**, and both were designed
for backends that cannot exploit the regularity they remove — a streaming decoder
with tiny state, or a fixed-rate channel. Against a BWT/LZMA-class backend the
removed redundancy was already nearly free, while the alignment cost is real.

This is now the **third** independent instance of the rule that has governed every
negative result in this project:

> **A transform pays only if it removes redundancy the backend cannot already
> capture. Otherwise it adds cost and removes nothing.**

delta, byte-shuffle, LZP, the word tokenizer, Gorilla and Rice are all instances.
The rule predicted them and can be used to reject future ideas before building them.

## 3. Size gate — the gain holds at every size tested

| rows | text | bin+delta | varint | best gain |
|---|---|---|---|---|
| 50 | 160 | 116 | **96** | **−40.0%** |
| 200 | 316 | 192 | **152** | −51.9% |
| 1,000 | 852 | 480 | **384** | −54.9% |
| 5,000 | 3,276 | 1,680 | **1,376** | −58.0% |
| 20,000 | 10,972 | 5,376 | **4,796** | −56.3% |
| 80,000 | 45,328 | 19,436 | **18,144** | −60.0% |

**No row-count gate is needed** — the column pays from 50 rows upward. What the
header cost actually scales with is the **number of columns**, not the number of
rows, so the gate should be on *file size versus column count* (a 2 KB file with 40
columns should decline), not on rows.

## 4. Block boundaries — a design constraint, not yet measured

If a column spans parallel blocks, the per-column decision and its metadata must
live in a **global header**, because a per-block decision could transform the same
column two different ways and the decoder could not tell. This is cheap to design in
now and expensive to retrofit — recorded here as a constraint on the implementation,
not as a measurement.
