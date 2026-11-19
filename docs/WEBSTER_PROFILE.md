# `webster` — PPMd owns it, and the knobs are not the answer

2026-11-19. `webster` (41,458,703 bytes, English text) is the worst remaining gap to bzip3, and the
one file where the BWT candidate never fires. This is the profile, the two knobs that were tried,
and why the gap is a design difference rather than a setting.

## What owns the blocks

`-9` at the 4 MiB default, block modes read from `ziptastic info`:

```
10 blocks, all PPMD          archive 7,158,216
```

All ten blocks, no exceptions. And the BWT candidate contributes exactly nothing here — measured two
ways: `-9` and `-9 --no-bwt` produce **byte-identical** archives on this file, and when a build was
made whose PPMd was *worse* (below), the selector fell back to BWT and produced **7,259,207** — so
that is also what BWT alone is worth on `webster` at 4 MiB, and it is 1.4% behind PPMd.

## The two knobs, both tried

PPMd's parameters are two constants in `src/entropy/ppmd.cpp`: `kOrder = 6` and `kMemSize = 16 MiB`.

| variant | `webster` at 4 MiB | vs default | blocks chosen |
|---|---:|---:|---|
| order 6, 16 MiB (default) | **7,158,216** | — | 10 × PPMD |
| order **8**, 16 MiB | 7,259,207 | **+1.41% worse** | 10 × BWT |
| order 6, **128 MiB** | 7,158,216 | **identical** | 10 × PPMD |

- **A higher order is worse.** Order 8 dilutes the statistics: PPMd then loses to BWT on every block,
  and the archive is 1.41% larger than the order-6 result it replaced. The selector did its job —
  both candidates were simply worse.
- **A bigger model changes nothing.** 128 MiB against 16 MiB produces byte-identical output, so the
  16 MiB model is not the constraint on a 4 MiB block.

That also settles a stale comment: `ppmd.cpp` says *"16 MiB of model, ample for 256 KiB blocks"* —
written when blocks were 256 KiB, and never revisited when the default moved to 4 MiB. The comment
is out of date; the constant, measured, is not wrong.

## Why the gap to bzip3 remains

| at | ziptastic | bzip3 -10 | gap |
|---|---:|---:|---:|
| 4 MiB blocks | 7,158,216 | 6,448,837 | +11.0% |
| 16 MiB blocks | 6,785,793 | 6,448,837 | +5.23% |
| 32 MiB blocks | 6,605,132 | 6,448,837 | +2.42% |

bzip3 wins with **BWT + arithmetic coding over context models**. ziptastic's BWT is libbsc (BWT +
QLFC), and on this file it loses to ziptastic's own PPMd. So there is no setting here that closes the
gap:

- re-coding libbsc's BWT output with a general-purpose coder is **worse** than QLFC — measured
  (`docs/BLOCK_SIZE_MEASURED.md`: xz +17.70%, zstd +25.67% on the same transform);
- the entropy stage is not the constraint — measured twice, above and in that document;
- the block size narrows the gap without closing it, and it is already policy above 8 MiB.

What would close it is **bzip3's coder family** — arithmetic coding with context modelling over the
BWT stream — which is a new backend and a format change, not a knob.

## The decision

**Not worth it for one file.** The remaining gap is 2.42% on `webster` at 32 MiB and 5.23% at the
policy's 16 MiB, on one file of twelve, for a new block format with a new decoder path. Recorded so
the question is not reopened as if it were a tuning problem — the tuning was done.

If a second file ever shows the same shape (PPMd-owned, BWT never fires, bzip3 ahead), the case
changes: that would be a content class rather than one file, and a class is worth a backend.

## Reproduce

```bash
ziptastic -9 -b 4096 -f -o w.dr "$CORPUS/webster"
ziptastic info w.dr | awk 'NR>12 && NF>=5 {print $5}' | sort | uniq -c   # 10 PPMD

# the knobs, in src/entropy/ppmd.cpp: kOrder (6 -> 8) and kMemSize (16 -> 128 MiB)
cmake --build build -j3 --target ziptastic
```

Every archive above was decompressed and compared byte for byte; the round trip is what the
`integration.corpus` suite re-checks after any change to these constants.
