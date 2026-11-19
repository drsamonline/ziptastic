# The BWT backend, measured — it fires, it pays, and it is still 7% behind bzip3

2026-11-19. The question `docs/HARNESS_MEASURED.md` left open: bzip3 beats ziptastic on all seven
text files of the Silesia corpus — is the selector failing to choose BWT where it should, or is
the BWT backend itself behind?

**The backend is behind, not the selector.** The BWT candidate fires and pays on six of the seven
files; bzip3 still wins every one of them.

## Method

`-9` against `-9 --no-bwt` on the same file. The difference is exactly what the BWT candidate
(libbsc) contributes, because every other candidate is untouched. Every archive in both arms was
decompressed and compared byte for byte. bzip3 1.5.4 `-10` figures are from the same corpus, same
files, measured in `docs/HARNESS_MEASURED.md`.

Seven files: the ones bzip3 wins.

| file | `-9` | `-9 --no-bwt` | BWT saves | bzip3 -10 | `-9` vs bzip3 |
|---|---:|---:|---:|---:|---:|
| `dickens` | 2,355,685 | 2,360,448 | 4,763 (0.20%) | 2,233,938 | +5.45% |
| `mr` | 2,249,704 | 2,365,814 | 116,110 (4.91%) | 2,119,994 | +6.12% |
| `nci` | 1,434,131 | 1,829,641 | **395,510 (21.62%)** | 1,366,367 | +4.96% |
| `osdb` | 2,368,404 | 2,495,870 | 127,466 (5.11%) | 2,261,858 | +4.71% |
| `reymont` | 1,024,711 | 1,156,716 | 132,005 (11.41%) | 980,463 | +4.51% |
| `webster` | 7,158,216 | 7,158,216 | 0 (0.00%) | 6,448,837 | **+11.00%** |
| `x-ray` | 3,798,464 | 4,056,779 | 258,315 (6.37%) | 3,657,087 | +3.87% |
| **total** | **20,389,315** | **21,423,484** | **1,034,169 (4.83%)** | **19,068,544** | **+6.93%** |

## What it says

1. **The selector is doing its job.** Removing the BWT candidate costs 4.83% across these seven
   files — 21.62% on `nci`. The per-block "smallest output wins" rule is choosing BWT wherever it
   wins, and BWT wins a lot on this content.
2. **The backend is 3.9–11.0% behind bzip3 on every file**, including the ones where BWT carries
   the block. So the gap is not selection; it is the BWT implementation's second stage. ziptastic's
   BWT candidate is **libbsc** (BWT + QLFC, a fast entropy coder); bzip3 pairs BWT with arithmetic
   coding over context models. Same transform, weaker coder behind it.
3. **`webster` is the outlier**: the BWT candidate contributes nothing (identical bytes with and
   without it), so another backend owns every block there — and ziptastic is still 11.00% worse
   than bzip3 on it, its worst gap on the seven. Whatever wins on `webster` is worth looking at
   next; it is the largest text file in the corpus and the one no BWT block reaches.

## What to do about it, cheapest first

| # | experiment | cost | what it answers |
|---|---|---|---|
| 1 | Run the libbsc output through the existing rANS/range stage (a second entropy pass on BWT blocks) | small — both coders are already in the tree | whether the gap is the entropy coder or the transform |
| 2 | Port bzip3's arithmetic-coded BWT block format behind the same `Mode::BwtBsc`-style enum value | medium — one new mode, one new decoder path, format version bump | whether matching the coder closes the 7% |
| 3 | Profile `webster`'s chosen backends | small | what beats BWT there, and whether it generalises |

Do **1** before **2**: if a second pass on libbsc's output recovers most of the gap, the transform
is fine and only the coder changes — a much smaller diff than a new block format.

## Reproduce

```bash
for f in dickens mr nci osdb reymont webster x-ray; do
  ziptastic -9        -f -o /tmp/a.dr "$CORPUS/$f"
  ziptastic -9 --no-bwt -f -o /tmp/b.dr "$CORPUS/$f"
  ziptastic -d -f -o /tmp/a.out /tmp/a.dr && cmp "$CORPUS/$f" /tmp/a.out   # both arms verified
done
```

## Caveats

- Seven files, all text-ish, chosen because they are the seven bzip3 wins — this is a targeted
  comparison, not a corpus sweep. The corpus-wide effect of the BWT candidate is smaller than
  4.83%; on the five files ziptastic already wins, BWT is not the deciding backend.
- One run per cell. Ratio is deterministic; the seconds in the probe are not reported here.
- bzip3 is 1.5.4 `-10` from lzbench 2.4; ziptastic is 1.4.5.
