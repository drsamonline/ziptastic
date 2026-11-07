# Building ziptastic

Written 2026-11-04. The recipe that actually works, because until now it lived only in a session
log and cost many hours to reconstruct.

## What you need

- **CMake ≥ 3.25**, **Ninja**, and a **C++20** compiler.
- No system packages beyond that: brotli, brunsli, libbsc, zpaq, ppmd, preflate and liblzma are
  vendored under `vendor/`.
- **~2 GB of RAM**; that is what every number in `FINDINGS.md` was measured on. It also reports two
  cores and delivers **one core of throughput** — two threads take as long as two sequential runs —
  so no thread claim in this repository has been tested. `FINDINGS.md` §0 has the measurement. If you
  are timing anything about `--parallel` or `-T`, check your own machine first.

## The normal path

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build          # 35 suites, ~4 minutes
./build/ziptastic version
```

That is the whole of it on a machine with a working toolchain and enough RAM to compile brunsli and
brotli at once.

## When there is no compiler at all

A container with no `g++`, no `cmake` and no `ninja`, and no root to install them, still builds
this: pip ships all three as wheels, including a complete C/C++ toolchain.

```bash
pip install --user ziglang cmake ninja
ZIG=$(python3 -c 'import ziglang,os;print(os.path.dirname(ziglang.__file__))')/zig
printf '#!/bin/sh\nexec %s cc  -target x86_64-linux-gnu "$@"\n' "$ZIG" > ~/.local/bin/zigcc
printf '#!/bin/sh\nexec %s c++ -target x86_64-linux-gnu "$@"\n' "$ZIG" > ~/.local/bin/zigcxx
chmod +x ~/.local/bin/zigcc ~/.local/bin/zigcxx

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=$HOME/.local/bin/zigcc \
      -DCMAKE_CXX_COMPILER=$HOME/.local/bin/zigcxx
```

Two things to know. The configure step alone takes **~5 minutes** here, because CMake compiles and
runs its feature probes one at a time through a wrapper that is slower than a native compiler.
And this is the toolchain the depfile note below is about: with zig wrappers, strip the depfiles
before the first build, and touch `src/` and `tests/` after any header change.

Building every target in one `cmake --build build -j3` on two cores will exceed a five-minute
command limit; the target-by-target loop below is the way through, and it resumes where it stopped.

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
rebuild of the sources that include it. That includes `src/engine/compressor.hpp`, which is where the
stats struct lives — editing it and rebuilding only the touched file leaves every other object
compiled against the old layout, and the first symptom is not a link error but a corrupted argument:
a `CompressOptions` with a block size of zero, returned as `bad-block-size` from a call that passed
it correctly. It cost a debugging session on 2026-11-04. After any edit under `include/` **or to a
`.hpp` under `src/`**, do:

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
ziptastic 1.2.0 (CHCE format v8)

$ head -c 200000 /dev/urandom > t.bin
$ ./build/ziptastic c -9 t.bin            # writes t.bin.dr
$ ./build/ziptastic d t.bin.dr            # writes t.bin, byte-identical
```

If `ctest` is green and that round trip is byte-exact, you have a working build.

The offset table has its own smoke test, and it is the one to run after touching the trailer:

```
$ ./build/ziptastic info t.bin.dr
...
blocks         1
block table    verified against the manifest digest, 108 bytes read of 118284
```

`verified` is doing work: `info` reads every block header at the position the table names and
re-hashes them into the manifest before printing anything.
