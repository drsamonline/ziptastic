# The field, measured — lzbench, and the codec that beats ziptastic on text

2026-11-19. Plan item 2 was recorded as "build lzbench in the same environment", blocked by the
missing `make`. It was never blocked either: **lzbench publishes prebuilt Linux binaries**
(`lzbench24_x86_64-linux-gnu-gcc15.gz` on the v2.4 release). Downloaded, gunzipped, run.

Every number below is on the same corpus — Silesia, 12 files, 211,938,580 bytes — and every
archive was decoded and compared byte for byte before its size was recorded. ziptastic's own
authority remains `docs/FINDINGS.md`; this file is the external field it is measured against.

## 1. The field, ranked by size

| codec, at its maximum useful level | archive | ratio of raw | vs ziptastic | median speed |
|---|---:|---:|---:|---:|
| **ziptastic -9** | **43,401,535** | **20.48%** | — | 0.26 MB/s |
| bzip3 1.5.4 -10 | 46,426,578 | 21.91% | +6.97% | 2.65 MB/s |
| xz -9e (liblzma) | 48,456,004 | 22.86% | +11.65% | 0.88 MB/s |
| 7z LZMA2 9e | 48,456,884 | 22.86% | +11.65% | 0.87 MB/s |
| brotli 1.1.0 q11 | 50,332,587 | 23.75% | +15.97% | 0.27 MB/s |
| zstd 1.5.7 -22 | 52,364,062 | 24.71% | +20.65% | 1.04 MB/s |
| OpenZL 0.3.0 serial -l9 | 62,902,274 | 29.68% | +44.93% | 11.8 MB/s |
| libdeflate 1.26 -12 | 64,681,628 | 30.52% | +49.03% | 3.14 MB/s |
| lz4hc 1.10.0 -12 | 77,262,399 | 36.46% | +78.01% | 8.20 MB/s |

ziptastic is **first on ratio and last on speed**: 0.26 MB/s, neck and neck with brotli q11
(0.27), and **45× slower than OpenZL** (11.8). That is the trade `docs/PARETO.md` describes; this
table is what the frontier looks like against the rest of the field rather than against its own
settings.

## 2. The finding: bzip3 wins seven files of twelve

Totals hide this. Per file, smallest in bold:

| file | ziptastic -9 | bzip3 -10 | xz -9e | zstd -22 | OpenZL serial |
|---|---:|---:|---:|---:|---:|
| `dickens` | 2,355,685 | **2,233,938** | 2,831,212 | 2,849,064 | 3,370,763 |
| `mozilla` | **12,353,529** | 15,832,996 | 13,376,240 | 14,907,805 | 18,527,245 |
| `mr` | 2,249,704 | **2,119,994** | 2,751,892 | 3,102,439 | 3,322,026 |
| `nci` | 1,434,131 | **1,366,367** | 1,449,272 | 1,579,583 | 2,581,745 |
| `ooffice` | **2,131,573** | 2,526,886 | 2,427,224 | 2,594,905 | 3,064,885 |
| `osdb` | 2,368,404 | **2,261,858** | 2,844,556 | 3,100,120 | 3,375,077 |
| `reymont` | 1,024,711 | **980,463** | 1,315,592 | 1,348,518 | 1,737,134 |
| `samba` | **3,737,179** | 3,918,960 | 3,739,524 | 3,868,408 | 4,767,623 |
| `sao` | **4,415,125** | 4,673,311 | 4,425,664 | 5,000,572 | 5,250,148 |
| `webster` | 7,158,216 | **6,448,837** | 8,368,672 | 8,431,509 | 10,745,352 |
| `x-ray` | 3,798,464 | **3,657,087** | 4,491,264 | 5,129,824 | 5,588,729 |
| `xml` | **374,814** | 405,881 | 434,892 | 451,315 | 571,547 |

**bzip3 is smaller on seven files: every text file** (`dickens`, `mr`, `nci`, `osdb`, `reymont`,
`webster`, `x-ray`). ziptastic wins the five where Layer 0 has something to do — `mozilla`,
`ooffice`, `samba`, `sao`, `xml` — and wins the corpus total on those five alone.

That is the same lesson OpenZL's `sao` profile taught, from the other direction: **ziptastic's
advantage is its transforms, not its entropy coder.** Where the transforms do not fire, a plain
BWT + arithmetic coder (bzip3) is 5–10% better on text, and 0.26 MB/s of context mixing buys
nothing.

## 3. What it changes

1. **A BWT backend is worth adding to the selector.** ziptastic already carries rANS, static
   Huffman, range, BWT, PPMd and LZMA2 — the per-block "smallest output" rule should pick BWT on
   these files if the BWT backend were competitive with bzip3's. It is not, and the gap is
   measurable on seven files. That is a concrete, bounded piece of work: compare the existing BWT
   backend against bzip3 on the same seven files.
2. **Speed is the unexploited axis, not ratio.** ziptastic wins ratio by 7% over the runner-up and
   is the slowest of the nine — 0.26 MB/s, against bzip3's 2.65 and OpenZL's 11.8. A fast mode has
   to come from a different backend, not a tuning flag.
3. **The field is now on record.** Nine codecs, one corpus, every archive verified — the first time
   this repo can say "best on ratio" with a table that includes the thing that beats it on half
   the corpus.

## 4. Reproduce

```bash
# lzbench: no make needed — take the prebuilt Linux binary from the v2.4 release
curl -sSL -o lzbench.gz \
  https://github.com/inikep/lzbench/releases/download/v2.4/lzbench24_x86_64-linux-gnu-gcc15.gz
gunzip -f lzbench.gz && chmod +x lzbench

./lzbench -e"zstd,22/lz4hc,12/libdeflate,12/bzip3,10" -o4 -t0,0 -i1,1 corpus/<file>
```

`-t0,0 -i1,1` is one pass per codec: the sizes are what matter here, and the default time loop
would run the slow ones for a minute each.
