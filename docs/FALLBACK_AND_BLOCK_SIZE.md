# Two files that did not move — the fallback bypasses the block loop

2026-11-19. `docs/BLOCK_SIZE_LARGE.md` recorded a caveat: `samba` and `sao` were **byte-identical**
at 4 MiB and 16 MiB blocks, "which suggests their blocks are dominated by something that does not
scale with block size — worth a look". Looked.

## What they are

Both files are a **single `LZMA2_FALLBACK` block covering the whole stream**, at either block size:

```
$ ziptastic info samba.4096.dr      # and samba.16384.dr — identical
blocks         1
  0    0x00000016 3737067    21603255   LZMA2_FALLBACK
```

`sao`, 7,251,944 bytes: one block, `LZMA2_FALLBACK`, 4,415,013. Same at both sizes.

The Tier-1 fallback (`docs/FINDINGS.md` §3f) is a **whole-stream** LZMA2 pass, not a per-block
candidate. When it wins, the block loop's output is discarded and `-b` has nothing to act on. That
is the whole explanation: the two files are not insensitive to block size, they never entered the
block loop.

## The fallback was the right call on both

Native path (the block loop, with the fallback removed), against what the fallback produced:

| file | native 4 MiB | native 16 MiB | fallback (shipped) | fallback is better by |
|---|---:|---:|---:|---:|
| `samba` | 4,009,686 | 3,991,578 | **3,737,179** | 6.4% / 6.8% |
| `sao` | 4,676,756 | 4,679,858 | **4,415,125** | 5.6% / 5.7% |

So there is no hidden win here: bigger blocks improve `samba`'s native path by 0.45% and make
`sao`'s 0.07% worse, and both remain 5–7% behind the fallback that shipped. On these two files
ziptastic *is* an LZMA-class compressor, which is why `HARNESS_MEASURED.md` shows it within 0.25%
of `xz -9e` on both and why bzip3 beats it there.

## What this corrects in the previous table

The "non-text, 4 files, +0.31%" row in `BLOCK_SIZE_LARGE.md` is really **two files**:

| group | 4 MiB | 16 MiB | change |
|---|---:|---:|---:|
| `samba`, `sao` (fallback, block size inapplicable) | 8,152,304 | 8,152,304 | 0.00% |
| `ooffice`, `xml` (block loop) | 2,506,387 | 2,538,927 | **+1.30%** |
| non-text as previously reported | 10,658,691 | 10,691,231 | +0.31% |

So the honest statement is: **16 MiB costs 1.30% on the two block-loop files outside text, and does
nothing at all to the two that are whole-stream LZMA2.** The corpus total (−2.70%) is unaffected —
those two files contribute the same bytes either way — but the *shape* of the trade is clearer, and
the +0.31% was diluted by files the change cannot touch.

## The general lesson for every table in this repo

A corpus table that mixes fallback files with block-loop files is measuring two different machines.
One number per file hides which one ran. `ziptastic info` prints the mode per block, and any table
about block size, backends or the selector should carry it — or at least name the files that never
reached the loop.

Worth adding to the reporting habit: **when a cell does not move, check whether the feature was
reached at all before concluding it did not help.**

## Reproduce

```bash
ziptastic -9 -b 4096  -f -o a.dr "$CORPUS/samba" && ziptastic info a.dr | grep -A1 '^blocks'
ziptastic -9 -b 16384 -f -o b.dr "$CORPUS/samba" && cmp a.dr b.dr && echo identical
ziptastic -9 -b 16384 --no-lzma2 -f -o c.dr "$CORPUS/samba"   # the native path, for comparison
```
