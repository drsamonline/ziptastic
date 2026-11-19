# Block size, at sizes where it can matter

2026-11-19. `docs/FINDINGS.md` §3d swept the block size and concluded 4 MiB stays — "it does not
move without a reason that is bigger than 0.6%". This is that reason, and it explains why §3d could
not find it.

## What §3d measured, and the hole in it

§3d swept 64 KiB → 16 MiB on four corpora: 1.6 MB, 846 KB, 1.4 MB and 2.75 MB. **Every one is
smaller than 16 MiB**, so the 16 MiB column was the same stream as the 4 MiB column — §3d says so
itself: *"Cells repeat where the block size exceeds the file."* The large-block regime had never
been measured on a file large enough to have one.

That is not a criticism of the sweep, it is what a corpus of small files can see. BWT is a
whole-block transform, so its gain is a function of how much data one sort sees; below 4 MiB there
is nothing to sort.

## The measurement, on files that have the regime

Silesia, `-9`, block size varied with `-b`, every archive decompressed and compared byte for byte.
The corpus is 5–51 MB per file, so 4, 16 and 32 MiB are three genuinely different streams.

| file | raw | 4 MiB | 16 MiB | 16 vs 4 |
|---|---:|---:|---:|---:|
| `dickens` | 10,192,446 | 2,355,685 | 2,260,672 | −4.03% |
| `mr` | 9,970,564 | 2,249,704 | 2,205,432 | −1.97% |
| `nci` | 33,553,445 | 1,434,131 | 1,280,531 | **−10.71%** |
| `osdb` | 10,085,684 | 2,368,404 | 2,247,164 | −5.12% |
| `reymont` | 6,627,202 | 1,024,711 | 982,707 | −4.10% |
| `webster` | 41,458,703 | 7,158,216 | 6,785,793 | −5.20% |
| `x-ray` | 8,474,240 | 3,798,464 | 3,756,678 | −1.10% |
| **text, 7 files** | **120,362,284** | **20,389,315** | **19,518,977** | **−4.27%** |
| `ooffice` | 6,152,192 | 2,131,573 | 2,156,380 | +1.16% |
| `samba` | 21,606,400 | 3,737,179 | 3,737,179 | 0.00% |
| `sao` | 7,251,944 | 4,415,125 | 4,415,125 | 0.00% |
| `xml` | 5,345,280 | 374,814 | 382,547 | +2.06% |
| **non-text, 4 files** | **40,355,816** | **10,658,691** | **10,691,231** | **+0.31%** |
| **all 11 measured** | **160,718,100** | **31,048,006** | **30,210,208** | **−2.70%** |

`mozilla` (51 MB) is excluded: a whole-file `-9` on it does not finish inside a 300-second step at
either size, which is why `docs/FINDINGS.md` §5 records it as two halves.

**32 MiB continues the trend** on the two largest text files:

| file | 4 MiB | 16 MiB | 32 MiB | 32 vs 16 |
|---|---:|---:|---:|---:|
| `webster` | 7,158,216 | 6,785,793 | 6,605,132 | −2.66% |
| `nci` | 1,434,131 | 1,280,531 | 1,208,977 | −5.59% |

## What it costs

On the seven text files, `-9` took **470 s at 16 MiB against 395 s at 4 MiB** — **+19%**, because a
block four times the size means four times the working set per candidate and fewer blocks in flight.
The 32 MiB runs are slower again. This machine cannot resolve time differences finer than that
(`FINDINGS.md` §3c), so treat +19% as the order of magnitude.

## Against bzip3

`docs/HARNESS_MEASURED.md` recorded bzip3 winning all seven text files. At 16 MiB ziptastic takes
`nci` (−6.28%) and `osdb` (−0.65%); at 32 MiB, `nci` is **−11.5%** against bzip3 and `webster`'s
deficit narrows from +5.23% to +2.42%.

## What to do

| option | gain | cost | verdict |
|---|---|---|---|
| keep 4 MiB, document `-b` | 0 | 0 | honest but leaves 2.70% on the table |
| raise the default to 16 MiB | −2.70% overall, −4.27% on text | +19% time; **+0.31% on non-text**; every holdout baseline must be re-recorded | a real trade, and it makes binary inputs slightly worse |
| **size-conditional: inputs above ~8 MiB get 16 MiB blocks** | the −4.27% on text where files are large, 0% elsewhere | one branch in the options default | **recommended** — and the holdout is untouched: its files are 21 KB–840 KB, far below the threshold |

The third option is the one that fits what the numbers say: the gain exists only where the file is
big enough to contain a big block, and no file in `holdout/` is. It is a small change in the options
default, but it changes archive bytes for large inputs, so it needs the holdout re-run and a CI pass
before it ships — which is why this document stops at the decision rather than the diff.

## Caveats

- Eleven files, one corpus, one run per cell. Sizes are exact; times are not.
- The non-text group is four files and two of them (`samba`, `sao`) are byte-identical at both
  sizes, which is suspicious in the useful sense: it means the block content there is dominated by
  something that does not scale with block size — worth a look before trusting the group's +0.31%.
- `-9` only. `--max-ratio` (CM) may behave differently, and no transform was disabled, so this is
  the shipped product at each size, not a backend isolation.
