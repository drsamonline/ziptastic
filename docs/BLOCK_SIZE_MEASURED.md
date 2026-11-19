# The BWT gap is the block size, not the coder

2026-11-19. `docs/BWT_MEASURED.md` left a ladder of three experiments and said do the cheap one
first: "run libbsc output through the existing rANS/range stage". It was done. **It cannot work**,
and the experiment that does work is the one nobody had on the list.

## 1. The second entropy pass — refuted, twice

**There is no residual redundancy in libbsc's output.** Compressing it again with zstd -22 adds
22 bytes to 374,656 — headers, not content. QLFC leaves nothing for a second stage to find.

| file | raw | libbsc (BWT+QLFC) | zstd -22 on libbsc's output |
|---|---:|---:|---:|
| `xml` | 5,345,280 | 374,656 | 374,678 (+0.01%) |
| `ooffice` | 6,152,192 | 2,565,414 | 2,565,487 (+0.00%) |

**Replacing QLFC with a general-purpose coder on the same BWT is much worse**, so QLFC is not the
weak link either. BWT output coded by xz -9e and zstd -22, against what libbsc's QLFC achieves on
the identical transform:

| file | libbsc QLFC | xz -9e on the BWT stream | zstd -22 on the BWT stream |
|---|---:|---:|---:|
| `xml` | **374,656** | 440,972 (+17.70%) | 470,836 (+25.67%) |
| `ooffice` | **2,565,414** | 2,778,980 (+8.32%) | 2,945,697 (+14.82%) |

Both halves of the "weaker coder" hypothesis are dead. Probes: `probe_bwt.cpp` (libbsc settings
exactly as the engine uses them) and `probe_bwt2.cpp` (`bsc_bwt_encode` alone, so an external coder
can be measured on the same transform). Both live in the audit repo at
`log/2026-11/2026-11-19-probes/`.

## 2. The block size — the actual cause

ziptastic's default block is **4 MiB** (`kDefaultBlockSize`). bzip3's default is far larger. BWT is
a whole-block transform: the bigger the block, the more context the sort can exploit. Measured on
the seven text files, `-9` at the default and at 16 MiB (`-b 16384`), every archive byte-verified:

| file | `-9` (4 MiB) | `-9` (16 MiB) | change | bzip3 -10 | 16 MiB vs bzip3 |
|---|---:|---:|---:|---:|---:|
| `dickens` | 2,355,685 | 2,260,672 | −4.03% | 2,233,938 | +1.20% |
| `mr` | 2,249,704 | 2,205,432 | −1.97% | 2,119,994 | +4.03% |
| `nci` | 1,434,131 | 1,280,531 | **−10.71%** | 1,366,367 | **−6.28%** |
| `osdb` | 2,368,404 | 2,247,164 | −5.12% | 2,261,858 | **−0.65%** |
| `reymont` | 1,024,711 | 982,707 | −4.10% | 980,463 | +0.23% |
| `webster` | 7,158,216 | 6,785,793 | −5.20% | 6,448,837 | +5.23% |
| `x-ray` | 3,798,464 | 3,756,678 | −1.10% | 3,657,087 | +2.72% |
| **total** | **20,389,315** | **19,518,977** | **−4.27%** | **19,068,544** | **+2.36%** |

The gap to bzip3 falls from **+6.93% to +2.36%**, and ziptastic **wins two of the seven** files it
used to lose: `nci` by 6.28% and `osdb` by 0.65%.

The cost is real: 470 s against 395 s on these seven files, **+19% time**, because a 16 MiB block
means four times the working set per candidate and fewer blocks in flight.

## 3. What this means

1. **The coder was never the problem.** Both experiments that targeted it — a second pass, and a
   stronger coder on the same transform — make things worse or do nothing. A bzip3-class block
   format (experiment 2 in the previous doc) would have bought almost nothing: the gap is not in
   the coding, it is in how much data each BWT sees.
2. **4 MiB is an unmeasured default.** It costs 4.27% on text against 16 MiB, and nothing in the
   docs justified it. That is the same class of defect as the README claim in faztest: a number
   nobody had tested standing in for a decision.
3. **`webster` is still the outlier** — 5.20% better at 16 MiB and still 5.23% behind bzip3, the
   worst gap remaining. It is the largest text file and the one where the BWT candidate is never
   selected at 4 MiB; at 16 MiB the picture changed enough to look again.

## 4. Next, in order

| # | experiment | why |
|---|---|---|
| 1 | Full Silesia at 4 / 8 / 16 / 32 MiB, all 12 files | one table decides the default; text gains 4.27%, but binary and container files may not, and the time cost is +19% |
| 2 | Decide whether the default moves, or `-b` is documented as the ratio lever | a default is a claim; it needs the table behind it |
| 3 | Re-run `PARETO.md`'s frontier with the block size as a third axis | block size is a ratio/time dial the frontier does not currently show |

Experiment 1 is the one that matters: if the gains hold across binary content, the default block
size is the cheapest 4% in the project.

## 5. Reproduce

```bash
# the coder experiments
g++ -O2 -I vendor/hcs/old/libbsc-master probe_bwt.cpp  <libbsc sources> -o probe_bwt
g++ -O2 -I vendor/hcs/old/libbsc-master probe_bwt2.cpp <libbsc sources> -o probe_bwt2

# the block-size experiment
ziptastic -9 -b 16384 -f -o /tmp/a.dr "$CORPUS/nci"
ziptastic -d -f -o /tmp/a.out /tmp/a.dr && cmp "$CORPUS/nci" /tmp/a.out
```
