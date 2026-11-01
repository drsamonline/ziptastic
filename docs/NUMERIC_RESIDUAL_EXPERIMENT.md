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
