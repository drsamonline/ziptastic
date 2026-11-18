# The holdout set

2026-11-18. Five measurements in one day were wrong for the same reason: each was taken on files the
author chose. The 64 KiB PNG sample, the formula-generated PNG, the gzip XML fixture, the synthetic
LZ4 frame, and the 99% column-filter saving that a formula database produced.

Discipline does not fix that. A split does.

## What it is

`holdout/` holds files that **no transform was designed against**, frozen with a SHA-256 manifest and
the archive size each one produced at the time of recording:

| file | bytes | archive |
|---|---|---|
| BENCHMARKS.md | 88,344 | 26,794 |
| FINDINGS.md | 93,232 | 27,516 |
| FORMAT.md | 21,326 | 7,095 |
| docs-archive.tar | 839,680 | 228,956 |

`scripts/holdout.py --check` verifies every hash still matches and re-measures; any file whose
archive **grew** against the recorded baseline is a regression in bytes, and the exit code is 1.

```bash
scripts/holdout.py --bin ./ziptastic --dir holdout --check
```

## The rule for what goes in it

Mechanical, so it is not taste:

1. **In the repository, and not in the development corpus.** The corpus is four PNGs and a PDF; the
   holdout is documentation, an archive of it, and — at release time — the built binary.
2. **Frozen.** A copy with a hash, not a path into `docs/`. A file that changes on every edit would
   report drift on every commit and teach everyone to ignore the check.
3. **Nothing here was chosen because a transform does well on it.** If a file was picked for its
   result, it belongs in the corpus, not here.

## What it catches, and what it does not

**Catches:** a change that quietly costs bytes on real text, an archive, or a binary — the class of
regression that a corpus tuned over months stops seeing.

**Does not catch:** a transform that is wrong in a way the corpus does not exercise. That is what
`integration.corpus` and the per-transform tests are for; this is the *cost* gate, not the
*correctness* gate.

**Also does not catch:** a file type nobody thought of. A holdout of four files is a floor, not a
census — it is the cheapest thing that would have caught today's errors, not a complete answer.

## At release time

The built binary is the best holdout available — a real ELF, which exercises the block loop on
incompressible-ish data — and it is an artifact rather than a repo file. So a release adds it to a
scratch copy of the set:

```bash
cp holdout/* /tmp/holdout-release/ && cp dist/ziptastic-* /tmp/holdout-release/
scripts/holdout.py --bin ./ziptastic --dir /tmp/holdout-release --record   # new baseline
```

A new release **records a new baseline**; it does not check against the old one, because the binary
changed. What the old baseline is for is catching a regression *between* releases on the same files.

## Current state

Recorded 2026-11-18 against `ziptastic 1.4.5`: **1,042,582 → 290,361 bytes (27.9%)**, check clean.
