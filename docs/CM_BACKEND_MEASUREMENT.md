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

## Why it is not simply switched on

`--max-ratio` applies CM to the whole stream, which means one giant model: fine for a
1 MB file, ruinous for a 128 MB image (the sandbox cannot even finish it in one call).
What it should be is a **per-block candidate in the selector**, so a 4 MiB block picks CM
where CM wins and the cheap backends where it does not — the same "sometimes 7z,
sometimes arc" answer the project already gives at block granularity.

That is the next piece of work:

1. `Mode::CM` alongside the existing modes, with zpaq's block API rather than its stream
   API.
2. A gate so it is not run on blocks where it cannot win — the `photoish.bmp` case, where
   the block is already a few hundred bytes, should cost nothing.
3. Measure per block on the corpus, and check the decode path.

## Status

Measured, not built. Recorded so the queue is honest: the handoff said "last, 1–3%" and
the measurement says "six to eight percent, sooner".
