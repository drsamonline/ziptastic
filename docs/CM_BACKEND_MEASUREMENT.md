# The CM backend is worth 6–8%, not the 1–3% it was budgeted at

Written 2026-11-02. `SESSION_HANDOFF.md` listed the context-mixing backend **last**, with
a 1–3% ceiling and the note "nobody publishes a CPU CM design at 5 MB/s". Measured
before building anything, and the ceiling is wrong by a factor of three.

## Measured

`--max-ratio` runs the vendored CM (zpaq) over the whole stream. Compared with `-9`:

| file | `-9` | `--max-ratio` | gain | time |
|---|---|---|---|---|
| 1 MB English prose | 206,324 | **189,899** | **−7.96%** | 30 s vs ~3 s |
| 900 KB 24-bit WAV | 689,374 | **646,406** | **−6.23%** | — |
| 786 KB photographic BMP | 1,375 | 1,375 | 0.00% | — |

The BMP is the sanity check that the measurement is not a rounding artefact: that file is
already 1,375 bytes from 786 KB, so there is nothing left for any backend to find, and CM
correctly changes nothing.

## What this changes

**CM is not the last item; it is one of the larger remaining wins.** At 6–8% on ordinary
text and audio it is worth more than every Layer 0 transform added this session except
PNG and ECM, and it needs no format parsing at all.

The cost is real: roughly 10× the wall time on this box, and the 1–3% figure in the
handoff was presumably taken from a corpus where the Layer 0 transforms had already
removed most of the redundancy. That is the lesson — **a backend's value depends on what
the front end left behind**, and measuring it on already-transformed data understates it.

## Correction: the per-block candidate already exists

The first draft of this document proposed building `Mode::CM` as a per-block selector
candidate. Reading the code before writing it showed that **it is already there** —
`encode_one_block` runs `zpaq_compress_block` per block under `--max-ratio` and pushes a
`Mode::CmZpaq` candidate like any other backend. What `--max-ratio` adds on top is a
**second, whole-stream pass**, and a comment in the source already records why:

> A CM model is stateful and long-lived; cutting it at a block boundary throws away
> everything it learned. Measured on xml: per-block `--max-ratio` gives 394,185 bytes
> while one CM pass over the same bytes gives 330,394 — 16% that the block boundaries
> were costing.

So the 6–8% measured above is the whole-stream pass, not the per-block candidate, and
"make it per-block" would be a step backwards. The real finding is about **block size**:
the default is 256 KiB (`-b`), so a 1 MB file is already four blocks and the model is cut
four times. The whole-stream pass exists to paper over that.

A 3 MB file of the same prose compresses to 189,932 bytes against 189,899 for 1 MB —
barely more for three times the input, because the stream pass sees the repetition across
block boundaries that the per-block candidates cannot.

## What that leaves

Two candidate pieces of work, neither built:

1. **A bigger default block size under `--max-ratio`** — the model's reach should not be
   256 KiB. A single 3 MiB block on the same file did not finish inside the sandbox's
   300 s call, so this is unmeasured rather than decided.
2. **Making the whole-stream pass affordable on large files** — it is one giant model, so
   a 128 MB image can never finish it here. Chunking it at a size the model can carry
   (rather than at 256 KiB) is the shape, and the number above says the chunk size
   matters a lot: 16% between 256 KiB and whole-stream on xml.

## Status

Measured, not built. The handoff said "last, 1–3%"; the measurement says "6–8%, sooner".
The follow-up is block sizing, not a new mode.
