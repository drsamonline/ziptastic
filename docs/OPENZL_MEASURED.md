# OpenZL, measured — the external check Layer 0 finally faced

2026-11-19. Plan item 1, blocked twice on "no compiler in the sandbox", is **done**. This file
records the build recipe, the numbers, and the one conclusion that changes how the fleet should
think about Layer 0.

The authority for ziptastic's own numbers stays `docs/FINDINGS.md`. This file is the external
comparison, and every figure in it comes from the tables below.

## 1. The build, and why it was never really blocked

The blocker was recorded as "`make` is not installed". That was the wrong diagnosis twice over:

| what was assumed | what is true |
|---|---|
| the CLI needs `make` | CMake drives **Ninja**; `make` is not needed at all |
| the sandbox cannot build it | the sandbox **can** — `ulimit -v` is 1,536,000 KiB with an *unlimited hard limit*, so `ulimit -v unlimited` lifts it (Vite 8 hit the same wall on 2026-11-19) |
| the cost is the SDDL compilers | the real blocker is **xgboost**: `tools/ml_selector` adds it as an `ExternalProject`, a full xgboost build, and `zli` does not link it |

Recipe that works here (2 vCPU, 2 GiB, no `make`, no gcc):

```bash
pip install ninja cmake ziglang
ZIG=$(python3 -c 'import ziglang,os;print(os.path.join(os.path.dirname(ziglang.__file__),"zig"))')
mkdir -p /scratch/work/tc
printf '#!/bin/sh\nexec %s cc "$@"\n'  "$ZIG" > /scratch/work/tc/gcc
printf '#!/bin/sh\nexec %s c++ "$@"\n' "$ZIG" > /scratch/work/tc/g++
chmod +x /scratch/work/tc/gcc /scratch/work/tc/g++

git clone --depth 1 -b release https://github.com/facebook/openzl.git   # 82 MB
cd openzl
# local patch, not upstream: comment out add_subdirectory(ml_selector) in tools/CMakeLists.txt
export CC=/scratch/work/tc/gcc CXX=/scratch/work/tc/g++
ulimit -v unlimited
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENZL_BUILD_TESTS=OFF -DOPENZL_BUILD_BENCHMARKS=OFF \
  -DOPENZL_BUILD_EXAMPLES=OFF -DOPENZL_BUILD_PYTHON_EXT=OFF -DOPENZL_INSTALL=OFF
ninja -C build zli -j2      # 484 objects; ~50 minutes, run in 300-second windows
./build/cli/zli --version  # Demo CLI for OpenZL. Version 0.3.0
```

`zli list-profiles` names 25 profiles: `serial`, `csv`, `sao`, `parquet`, `pytorch`, `zstd`,
`lz`, the integer-endian families (`le-u16`, `be-i64`, …), `sddl`, `sddl2`, and others.

## 2. `serial` — no format knowledge, the like-for-like comparison

`zli compress -p serial -l 9` against `ziptastic -9` and `xz -9e`, Silesia, every archive
decompressed and compared byte for byte.

| file | raw | OpenZL serial -l9 | ziptastic -9 | xz -9e | OpenZL vs ziptastic | OpenZL vs xz |
|---|---:|---:|---:|---:|---:|---:|
| `dickens` | 10,192,446 | 3,370,763 | 2,355,685 | 2,831,212 | +43.09% | +19.06% |
| `mozilla` | 51,220,480 | 18,527,245 | 12,353,529 | 13,376,240 | +49.98% | +38.51% |
| `mr` | 9,970,564 | 3,322,026 | 2,249,704 | 2,751,892 | +47.67% | +20.72% |
| `nci` | 33,553,445 | 2,581,745 | 1,434,131 | 1,449,272 | +80.02% | +78.14% |
| `ooffice` | 6,152,192 | 3,064,885 | 2,131,573 | 2,427,224 | +43.79% | +26.27% |
| `osdb` | 10,085,684 | 3,375,077 | 2,368,404 | 2,844,556 | +42.50% | +18.65% |
| `reymont` | 6,627,202 | 1,737,134 | 1,024,711 | 1,315,592 | +69.52% | +32.04% |
| `samba` | 21,606,400 | 4,767,623 | 3,737,179 | 3,739,524 | +27.57% | +27.49% |
| `sao` | 7,251,944 | 5,250,148 | 4,415,125 | 4,425,664 | +18.91% | +18.63% |
| `webster` | 41,458,703 | 10,745,352 | 7,158,216 | 8,368,672 | +50.11% | +28.40% |
| `x-ray` | 8,474,240 | 5,588,729 | 3,798,464 | 4,491,264 | +47.13% | +24.44% |
| `xml` | 5,345,280 | 571,547 | 374,814 | 434,892 | +52.49% | +31.42% |
| **total** | **211,938,580** | **62,902,274** | **43,401,535** | **48,456,004** | **+44.93%** | **+29.81%** |

| | ratio of raw | time over the corpus |
|---|---:|---:|
| OpenZL serial -l9 | 29.68% | **18.0 s** |
| ziptastic -9 | **20.48%** | 622.4 s |
| xz -9e | 22.86% | 241.2 s |

Read it plainly: on bytes with no format knowledge, **ziptastic is 31% smaller than OpenZL and
35× slower**; xz is 24% smaller and 13× slower. OpenZL `serial` is a fast, mediocre ratio, which
is what it is designed to be.

`mozilla` is the one cell that is not a whole file for ziptastic — measured as two halves, as
`docs/FINDINGS.md` §5 records, with `xz` on the same halves. OpenZL compressed it whole.

## 3. The part that matters: a format-aware profile wins outright

The corpus contains one file OpenZL has a dedicated profile for — `sao`, from which the corpus
takes its name. `zli compress -p sao -l 9`:

| sao, 7,251,944 bytes | archive | vs ziptastic -9 | vs xz -9e | time |
|---|---:|---:|---:|---:|
| OpenZL `-p sao` -l9 | **2,358,130** | **−46.59%** | **−46.72%** | **0.574 s** |
| OpenZL `-p serial` -l9 | 5,250,148 | +18.91% | +18.63% | 0.67 s |
| ziptastic -9 | 4,415,125 | — | −0.24% | 34.26 s |
| xz -9e | 4,425,664 | +0.24% | — | 5.66 s |

**46.6% smaller than the fleet engine, in 1/60th of the time.** That is not a tuning difference;
it is what knowing the format is worth. ziptastic's Layer 0 detects PNG, BMP, CSV/TSV, SQLite,
WAV, ZIP, gzip and base64 — it does not know SAO, and `serial` is what you get when nothing
matches.

Level barely moves `serial`: on `sao`, `-l 6` gives 5,294,308 against `-l 9`'s 5,250,148 (0.8%),
in 0.263 s against 0.67 s. The profile choice dominates the level.

## 4. What this changes

1. **The claim that survives.** "ziptastic beats xz and 7z on ratio" survives — 20.48% against
   22.86%, on a corpus that is text and binaries. The claim that does not survive, and was never
   tested until now, is that a **generic** engine is the right shape: on a format OpenZL has a
   profile for, it loses by half.
2. **The next transform to add is not another codec.** It is `sao` — a container the fleet's
   detectors do not cover, worth 46.6% on 3.4% of this corpus. The Layer-0 pattern that works is
   the one OpenZL demonstrates: identify the format, then delta within its structure.
3. **The speed column is not a footnote.** 18 s against 622 s on 211 MB means ziptastic's ratio
   is bought at 35× the time. Any product that ships `-9` as a default is choosing ratio over
   latency by a factor no user was shown. `docs/PARETO.md` already frames this; OpenZL supplies
   the missing point on the frontier.
4. **`make` was never the blocker.** Recorded here so the next session does not lose another two
   attempts to it: Ninja works, the `ulimit -v` cap is soft, and the xgboost `ExternalProject` is
   the thing to skip.

## 5. Reproduce

```bash
ulimit -v unlimited
bash scripts/openzl-bench.sh /path/to/silesia/corpus   # prints the serial table
```

The harness lives in `scripts/openzl-bench.sh`; it verifies every round trip and refuses to
report a size from an archive it could not decode.
