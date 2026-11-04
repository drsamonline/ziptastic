> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# Where the try-everything selector spends its time

Written 2026-11-02, before attempting to parallelise it. Measured on a 1 MB English
prose file, `-9`, one run per configuration on this 2-core box.

| configuration | time | size | note |
|---|---|---|---|
| default | 3,810 ms | 206,324 | |
| `--no-bwt` | 3,787 ms | 211,600 | BWT costs **0.6% of time** and **wins 2.6%** |
| `--no-huffman` | 3,135 ms | 206,324 | Huffman costs **18% of time** and wins **nothing** |
| `--no-bwt --no-huffman` | 2,769 ms | 211,600 | |
| `--no-lzma2` | 2,522 ms | 206,324 | the Tier-1 fallback costs **34%** and wins nothing *here* |

## What this says about parallelising

The proposal was to parallelise the candidate fan-out. On this evidence that is the
wrong lever for this machine: the candidates that cost time are **Huffman (18%) and the
LZMA2 fallback (34%)**, and on two cores a fan-out buys at most 2x on a stage that is
already partly serial. Worse, both of those spend their time to produce nothing on this
file — the output is byte-identical with and without them.

BWT, which the first (invalid) profile appeared to blame, is nearly free here and worth
2.6% of size. Keeping it is correct.

## The measurement that went wrong first, and why it is written down

The first profile reported "BWT costs 99.8% of the run and produces nothing". It was
wrong twice over:

1. `--no-bwt` **did not exist** as a CLI flag — it was only a `CompressOptions` field.
   The run failed with exit code 2 and the timing measured the failure, not a
   compression.
2. The output path was reused, so the size column showed the *previous* run's file.
   A failed run therefore looked like a successful one that produced identical output
   in 6 ms.

Two cheap habits would have caught it immediately, and both are now in the CLI: check
the exit code, and use a distinct output file per configuration. `--no-bwt` and
`--no-huffman` exist as flags now for exactly this reason.

## What the next step should be

Not parallelisation. The two candidates that cost time are worth *gating*, and the gate
has to be a measurement rather than a rule — three predictors in this project have
already been wrong in the direction that costs ratio:

- **Huffman**: 675 ms to lose on every block of a text file. A sample-based trial (the
  same shape as `bcj_helps` and `precomp_helps`) would settle it per block.
- **LZMA2 fallback**: 1,288 ms to lose here. It is a *guarantee*, not an optimisation —
  the promise that the stream is never worse than `xz`-class — so it should stay, but
  the comparison could run on a sample first and only fall back to a full run when the
  sample is close.

**Update, 2026-11-04: the Huffman gate is built.** `huffman_may_win` encodes a 64 KiB prefix,
scales it to the block, and runs the full candidate only if that estimate is within 1% of the best
candidate so far. No archive size changed on any of six corpora, and the seven-symbol file that
Huffman wins on still gets it (`FINDINGS.md` §3e). The 18% saving is not re-claimed here: this
machine cannot resolve it, and the profile above was taken on a different file with an older build.

**Update, 2026-11-04: the LZMA2 gate is built too, with a 20% margin.** A 64 KiB slice is encoded
with the real encoder, scaled, and the full run happens unless that estimate is more than 20% worse
than the native stream — the margin is the safety argument, and it is free: at 5% the gate skipped
exactly the same corpora. `FINDINGS.md` §3f has the table and the 256 KiB trial that had to be
abandoned because liblzma's setup, not its coding, is what a trial pays for.
