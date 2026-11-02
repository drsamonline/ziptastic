# Running the Tier-1 fallback concurrently with the native encode: measured, reverted

Written 2026-11-02. The native encode and the LZMA2 fallback are **independent** — both
encode the same bytes and the smaller stream wins — so running them on two threads turns
a sum into a max. That is the textbook win, and on this machine it loses.

## Measured, 1 MB of English prose, `-9`, two runs each

| configuration | time | size |
|---|---|---|
| native only (`--no-lzma2`) | 1,657 ms | 205,416 |
| sequential (shipped) | 3,368 ms | 205,416 |
| concurrent (two threads) | **3,681 ms** | 205,416 |

Sequential pays 1,711 ms for the fallback. Concurrent should have cost about
`max(1,657, ~1,711) = 1,750 ms` and cost **3,681 ms instead — 9% worse than sequential**.

## Why

The two encoders are individually about 1.7 s each, so a clean overlap would halve the
stage. Instead the threads effectively serialise and then add overhead, which is the
signature of contention rather than scheduling: LZMA2 at preset 9 wants a large
dictionary, the native path wants its own match finder, and this box has **2 cores and
2 GiB of RAM**. The working sets do not fit together, so the cache and memory system
serialises what the scheduler tried to overlap.

Nothing was shared between the threads — separate buffers, separate libraries, read-only
input — so this is not a locking bug. It is a resource fit problem.

## What shipped

Nothing. The change was reverted. The measurement is recorded because the idea is
correct and will be right on a machine with more memory: the same patch on a box where
both working sets fit would turn 3.4 s into ~1.8 s. Worth retrying when the test machine
changes, and worth knowing that "two independent jobs, two threads" is not automatically
a win.

## How it was measured, and one trap

Each configuration ran twice after a warm-up, with a distinct output path per run — the
trap from `SELECTOR_PROFILE.md`, where a reused path let a *failed* run look like a
6 ms success with identical output. Sizes are identical across all three configurations,
which is the check that the concurrency changed nothing but timing.
