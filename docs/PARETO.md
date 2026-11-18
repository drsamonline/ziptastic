# The frontier, not a number

2026-11-18. Every table in this repo until now quoted one size per file. A size alone hides a
choice: the same file at three settings is three different products, and which is "best" depends on
whether you are paying for bytes or for seconds.

`scripts/pareto.py` measures every setting on every file and keeps only the points nothing else
beats on **both** axes. Run it:

```bash
scripts/pareto.py --bin ./ziptastic --corpus /path/to/dir
```

## Measured, on the four-class corpus

`ziptastic 1.4.5`, one run per cell, wall-clock seconds.

| file | setting | bytes | seconds | on the frontier |
|---|---|---|---|---|
| diagram.png | **-9** | **938** | **5.60** | ★ |
| diagram.png | --max-ratio | 938 | 11.73 | |
| diagram.png | --png-fast | 2,155 | 0.21 | |
| diagram.png | (no flags) | 2,155 | 0.18 | ★ |
| palette.png | **--max-ratio** | **9,886** | **5.23** | ★ |
| palette.png | **-9** | **13,831** | **1.58** | ★ |
| palette.png | --png-fast | 17,035 | 0.56 | |
| palette.png | (no flags) | 17,035 | 0.36 | ★ |
| photo.png | **--max-ratio** | **94,071** | **18.85** | ★ |
| photo.png | **-9** | **97,504** | **5.24** | ★ |
| photo.png | **--png-fast** | **115,774** | **0.64** | ★ |
| photo.png | (no flags) | 115,774 | 0.76 | |
| ui.png | **--max-ratio** | **41,486** | **61.44** | ★ |
| ui.png | **-9** | **51,822** | **25.33** | ★ |
| ui.png | **--png-fast** | **86,626** | **0.64** | ★ |
| ui.png | (no flags) | 86,626 | 0.66 | |
| photo.pdf | any | 17,594 | 0.47–1.03 | one point — the choice is made for you |

## Three things this shows that a single number could not

**1. `--max-ratio` is strictly dominated on `diagram.png`.** It produces **the same 938 bytes** as
`-9` and takes **twice as long** (11.73 s against 5.60 s). The CM backend bought nothing on that
file and charged six seconds for it. Anyone quoting "max-ratio is the smallest" is right about the
corpus average and wrong about this file.

**2. `photo.pdf` is setting-independent.** Four settings, one size: 17,594 bytes. The PDF's inner
JPEG is already compressed, so no setting changes it, and the frontier collapses to a single point.
That is a fact about the *content*, and it is invisible in a per-setting table.

**3. Bare `bench` is not the default product.** `(no flags)` matches `--png-fast` byte-for-byte in
every row, because `CompressOptions::precomp` defaults to **false** and only `-9` / `--max-ratio`
turn precompression on. So `ziptastic bench file.png` reports **86,626** where `ziptastic -9 file.png`
produces **51,822** — 41% worse — and nothing in the output says so.

That last one is a documentation trap rather than a bug, and it is now stated in `--help`.

## Why the frontier is the honest report

The 2026 AIT Data Compression Challenge ranked 117 compressors this way and found that *no single
compressor dominates across all criteria*. The same is true of one compressor's own settings, as
`ui.png` shows: three settings, three points, and no argument about which is "best" without saying
what you are buying.

**A claim that cites a size should cite the seconds it was bought at.** This table is the format for
that.

## Reproducing

The numbers move with the machine — `ui.png` at `--max-ratio` took 61 s here, and a faster CPU will
report less. The **shape** of the frontier is the stable part: which settings dominate which is a
property of the file, not of the hardware. Re-run `scripts/pareto.py` on your own machine before
quoting a second count.
