# Benchmarks

Results are logged after every phase, as required by `AGENT_PROMPT.md` §11.
"Measured, not assumed" — every number below came from a run on the machine
described under *Test machine*, with the command shown.

## Test machine

| | |
|---|---|
| CPU | containerised x86-64 vCPU, shared |
| OS | Debian 12 (bookworm) |
| Compiler | Clang 22.1.8 (via Zig 0.17.0 as the C/C++ driver), `-O3` |
| Build | CMake 4.4.4, Ninja, `-DCMAKE_BUILD_TYPE=Release` |

Absolute MB/s here is a floor, not a target: the container's CPU is shared and
the I/O path is a network-backed filesystem. Ratios are the meaningful column.

## Phase summary

| Phase | Silesia total | `silesia.zip` | Decode MB/s | Status |
|---|---|---|---|---|
| 0 — Skeleton (RAW only) | n/a | n/a | 100–108 | **pass** |
| 1 — LZMA2 fallback | | | | not started |
| 2 — Fine match finder | | | | not started |
| 3 — Joint parser | | | | not started |
| 4 — rANS + context models | | | | not started |
| 5 — Adaptive tables | | | | not started |
| 6 — Coarse index | | | | not started |
| 7 — Huffman modes | | | | not started |
| 8 — Semantic precompression | | | | not started |
| 9 — BWT / Brotli / Zstd / LZ4 | | | | not started |
| 10 — CM backend | | | | not started |
| 11 — PPMd | | | | not started |
| 12 — Optimization | | | | not started |

Reference targets (from §9): `ziptastic -9 corpus/` ≤ `xz -9 corpus/`;
`ziptastic -9 silesia.zip` ≤ 44.0 MiB; `ziptastic --max-ratio silesia/`
≤ 42 MiB; decode ≥ 100 MB/s.

## Phase 0 — Skeleton

**Scope.** CMake build, CLI (`compress` / `decompress` / `bench` / `version` /
`help`), v1 header serialisation, RAW blocks only. Nothing is compressed yet:
this phase proves the container and the block loop, not the ratio.

**Tests.** All five suites pass (`ctest --test-dir build --output-on-failure`):

| suite | what it proves | result |
|---|---|---|
| `unit.varint` | LEB128 round-trip, truncation, overlong encodings, max value, reader bounds | pass |
| `unit.header` | header round-trip; mutation of magic / version / flags / counts / modes / sizes each yields the specific error | pass |
| `unit.roundtrip` | 100 random files 0 B–1 MB plus block-boundary sizes, bit-exact; determinism; truncation at every prefix rejected | pass |
| `unit.fuzz` | 1000 mutated streams + header attacks + garbage: no crash, no hang, invariants hold | pass |
| `integration.cli` | `echo hi \| ziptastic \| ziptastic -d`, file round-trip, overwrite guard, error exits, `-b`, `bench` | pass |

**Container overhead.** A 1 MiB incompressible input produces 1 048 614 bytes:
38 bytes of header and block framing over 4 blocks of 256 KiB (9 bytes of
stream header + 6 bytes per block header + varint sizes). Overhead is
independent of content, so it is invisible on real inputs.

**Throughput (RAW, 64 MiB incompressible input).**

| operation | measurement |
|---|---|
| compress | 284 MB/s |
| decompress (incl. file read + write) | 100–108 MB/s over three runs |
| round-trip | bit-exact (`cmp`) |

RAW decode is a memory copy, so this figure is the I/O floor, not the engine's
ceiling; Phase 4 sets the real rANS decode number against the ≥150 MB/s gate.

**Notes and decisions.**

- Block size defaults to 256 KiB per §12 of the prompt; the decoder learns
  block sizes from each block header, so `-b` output decodes with any build.
- The stream header carries `original_size` and `block_count`, so the encoder
  buffers its input before writing. Documented in `docs/FORMAT.md`; a
  streaming trailer variant is deferred to Phase 12 if it ever matters.
- The build has no dependencies beyond CMake and a C++20 compiler, and links
  nothing from `vendor/hcs/` yet — that starts in Phase 1.
