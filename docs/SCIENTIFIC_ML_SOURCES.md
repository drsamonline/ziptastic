# Scientific / ML / GPU compression sources: what they add

Five more repositories added to `vendor/hcs/old/`. This batch is a different
angle from the repack tools: format-aware frameworks, a readable reference
implementation, and scientific lossy codecs.

## Licences

| repo | licence | usable? |
|---|---|---|
| **openzl** (Meta) | **BSD** | **yes** |
| **compressor** (nadavrot) | **Apache-2.0** | **yes** |
| **SPERR** (NCAR) | **Apache-2.0** | yes, but see below |
| ICS23-GPULZ | none | no — idea only |
| ijleesw/data-compression | none | no |

---

## 1. OpenZL (Meta, BSD) — the architecture we have been building by hand

OpenZL takes **a description of your data** and builds a specialised compressor
from it, with a single universal decompressor. It is the generalisation of the
Layer 0 chain this project already has (precomp, JPEG, GIF, dedup), and it is
BSD-licensed so its design can be adopted directly.

The concrete part worth reading is `custom_parsers/`:

```
csv   parquet   gguf   onnx   pytorch_model   safetensors   sddl   zip_lexer
```

These decompose structured formats into **typed streams** — one per column or
tensor — and then compress each with a codec chosen for that stream's type.

**Why this matters, and where it differs from what we measured.** Our earlier
negative result was that byte-shuffle before BWT loses (+4.8% to +27.6%). That
result still stands, but it tested *blind* reordering with a *single* backend.
Typed decomposition is a different proposition: a monotonically increasing integer
column gets delta coding, a categorical column gets FSE, a float tensor gets
byte-plane separation — each stream with its own codec, chosen per stream rather
than guessed per file. **That is the honest way to revisit numeric data**, and it
is the one place our earlier conclusion may not generalise.

**Implementable in the engine?** The *parsers* are format-specific and large. The
*idea* — split a typed stream, choose a codec per stream — is implementable and is
the natural next step for CSV/Parquet-like inputs.

**Implementable in the orchestrator?** Yes, and more cleanly: a parser registry
that recognises a format, decomposes it into typed streams, and hands each stream
to the existing pipeline. That is OpenZL's design, and our plugin ABI already has
the right shape.

## 2. nadavrot/compressor (Apache-2.0) — a readable reference, with one caveat

4,808 lines of Rust, documented in `docs/details.md`, covering:

- an LZ matcher with a **look-ahead parser** and a **multi-way cache**;
- a **Finite State Entropy (FSE)** encoder — the zstd/lzfse family, i.e. our
  catalogue item #2;
- an adaptive arithmetic encoder with **Bitwise** and **DMC (Dynamic Markov
  Compression)** predictors.

**The caveat, stated plainly:** the README says the two predictors are "mixed to a
single prediction", but `src/models/mixer.rs` is **34 lines and averages them**
(`p0 / 2 + p1 / 2`). It is *not* the learned logistic mixing that makes PAQ strong.
So this repo does **not** solve catalogue item #1; it is a starting point, and
anyone reading it expecting PAQ-grade mixing will be disappointed.

What it genuinely provides:
- **FSE** — a working, permissively licensed reference for item #2. This is the
  single most useful thing in this batch.
- **DMC** — a model type we do not have at all, in 233 readable lines.
- A **multi-way cache matcher** with look-ahead, worth comparing against our BT4.

Apache-2.0 is MIT-compatible, so this can be ported with attribution rather than
clean-roomed.

## 3. SPERR (NCAR, Apache-2.0) — lossy, so not for this engine

Wavelet + SPECK coding for scientific fields, with a rate-distortion target. It is
**lossy by design**; its lossless path is zstd. ziptastic is a lossless archiver,
so SPERR is out of scope — recorded here so nobody re-evaluates it later. Its value
would be in a *different* product that trades fidelity for ratio.

## 4. ICS23-GPULZ — idea only, no licence

GPU-parallel LZ77. No licence file, so no code may be used. The idea — parallel
match finding on a GPU — is relevant to the parallel mode's ceiling, but our
bottleneck is the entropy coders, not match finding, so the expected payoff is
low even if it were usable.

## 5. ijleesw/data-compression — no licence

A collection (234 MB) with no licence file. Documentation value only.

---

## What this changes

1. **Catalogue item #2 (FSE) now has a reference implementation** that is Apache-2.0
   and 433 lines. That moves it from "implement from a paper" to "port and verify",
   which is materially cheaper and lower-risk.
2. **Catalogue item #1 (mixing) does not.** nadavrot's mixer is an average. The
   real work still needs Mahoney's description of logistic mixing and SSE.
3. **A new item for the list: typed stream decomposition**, from OpenZL's parsers.
   It is the only credible way to revisit numeric and tabular data after our
   byte-shuffle result, and it belongs in the orchestrator rather than the engine.
4. **OpenZL validates the architecture** we arrived at independently: a registry of
   format-aware transforms behind one universal decompressor. That is worth knowing
   before building the orchestrator — we are not inventing a shape, we are
   implementing a known-good one.
