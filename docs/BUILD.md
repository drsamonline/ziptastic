# Building ziptastic

Written 2026-11-04. The recipe that actually works, because until now it lived only in a session
log and cost many hours to reconstruct.

## What you need

- **CMake ≥ 3.25**, **Ninja**, and a **C++20** compiler.
- No system packages beyond that: brotli, brunsli, libbsc, zpaq, ppmd, preflate and liblzma are
  vendored under `vendor/`.
- **~2 GB of RAM and 2 cores are enough**; that is what every number in `FINDINGS.md` was measured
  on. More cores only help `--parallel`.

## The normal path

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 32 suites, ~2 minutes
./build/ziptastic version
```

That is the whole of it on a machine with a working toolchain and enough RAM to compile brunsli and
brotli at once.

## If the build is slow, runs out of memory, or the compiler is odd

Two things this project actually hit, and what to do about them.

### 1. Build the vendors one at a time

A single `cmake --build build -j3` on 2 cores can exceed a 5-minute command limit and lose its
place. Building target by target resumes cleanly:

```bash
for t in brotli_vendor brunsli_vendor bsc_vendor zpaq_vendor ppmd_vendor \
         liblzma preflate_vendor preflate_shim chce_core ziptastic; do
  cmake --build build -j3 --target "$t" || break
done
```

### 2. If you use a zig-based toolchain, strip depfiles before building

With `CMAKE_C_COMPILER`/`CXX_COMPILER` pointed at zig wrappers, ninja's dependency log can be
written in a form it then refuses to parse, and the build stops with a corrupt-log error. Removing
the depfile declarations from the generated rules fixes it:

```bash
python3 - <<'PY'
import re
p = 'build/CMakeFiles/rules.ninja'
s = open(p).read()
s = re.sub(r'^  deps = gcc\n', '', s, flags=re.M)
s = re.sub(r'^  depfile = .*\n', '', s, flags=re.M)
open(p, 'w').write(s)
PY
rm -f build/.ninja_deps build/.ninja_log
```

**The cost of that fix is real**: with depfiles stripped, a change to a *header* does not trigger a
rebuild of the sources that include it. After any edit under `include/`, do:

```bash
find src tests -name '*.cpp' -exec touch {} +
cmake --build build -j3
```

A stale binary built against an old header has cost this project three separate debugging
sessions, twice presenting as a logic bug in code that was correct.

## Cross-compiling

Nothing in the tree is platform-specific except the thread pool and the CPUID dispatch in
`sha256.cpp`. Windows and Linux are what has been built; macOS has not been tried and is not
supported.

## What "it works" looks like

```
$ ./build/ziptastic version
ziptastic 1.0.0 (CHCE format v7)

$ head -c 200000 /dev/urandom > t.bin
$ ./build/ziptastic c -9 t.bin            # writes t.bin.dr
$ ./build/ziptastic d t.bin.dr            # writes t.bin, byte-identical
```

If `ctest` is green and that round trip is byte-exact, you have a working build.
