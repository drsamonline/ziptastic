Reverse: `heavenzip decompress report.pdf.hzpkg` reads the manifest, runs
`ziptastic -d` then `preflate --reverse`, restores `report.pdf`
bit-exactly.

**Key insight:** heavenzip never opens a compressor. It just spawns
processes. That is why it can be GPL-safe even while spawning GPL tools:
there is no linking, only process boundaries.

---

## 5. Hard Rules (Global — Apply Everywhere)

These rules apply across all four repos. Violating any of them breaks the
architecture.

### DO

- ✅ Keep each repo single-purpose. If a file doesn't belong, don't add it.
- ✅ Follow the repo-specific `AGENT_PROMPT.md` phases in order. Do not
  skip phases.
- ✅ Pass every phase's exit tests before starting the next phase.
- ✅ Commit with clear phase-tagged messages (`phase-<N>: <description>`).
- ✅ Update `docs/BENCHMARKS.md` (ziptastic) or `README.md` phase table
  (heavenzip, compfactory) after every phase.
- ✅ If a phase gate fails, commit as `phase-<N>: FAILED — <reason>` and
  stop. Report the failure.
- ✅ Treat `vendor/hcs/` as read-only.
- ✅ Keep licenses compatible. If unsure, ask before linking.
- ✅ Keep offline at runtime. No network calls, no downloads, no models.

### DON'T

- ❌ Do not shell out to external executables from `ziptastic`. Compression
  must be linked.
- ❌ Do not link GPL/LGPL code into `ziptastic` (`paq8px`, `upx`,
  `advancecomp`, `packJPG`, `lzo`). They are external-process-only in
  `heavenzip`.
- ❌ Do not implement compression algorithms in `heavenzip`. heavenzip is a
  router, nothing more.
- ❌ Do not commit `build/`, `workspace/`, `*.exe`, `*.o`, or any build
  artefact. Every repo has a `.gitignore` — trust it.
- ❌ Do not add network dependencies, model downloads, or GPU paths.
- ❌ Do not `git subtree add` in compfactory. compfactory uses plain clones.
- ❌ Do not attempt to make one repo do another repo's job. The boundaries
  are the safety mechanism.
- ❌ Do not modify upstream sources under `HCS/old/`. If a source needs a
  patch, wrap it in `ziptastic/src/codecs/`.

---

## 6. License Strategy

You are making these repos **public**. That makes license discipline visible
and non-negotiable.

### Recommended `LICENSE` files (per-repo)

| Repo | Recommended license | Rationale |
|---|---|---|
| `HCS` | CC0 / Public Domain, or MIT | HCS is a mirror. You don't claim authorship of code inside `old/`. |
| `ziptastic` | MIT or Apache-2.0 | Permissive, matches the sources you actually link. |
| `heavenzip` | MIT | Same. |
| `compfactory` | MIT | Same. |
| `awesome-compressor` (future) | CC0 | A catalog should be freely forkable. |

Add a `LICENSE` file via GitHub UI (**Add file → Create new file → type
`LICENSE` → GitHub offers a template picker**), or via CLI. Add a
`## License` paragraph to each `README.md`.

### License classes inside HCS

HCS vendors 50+ sources across several licenses. They fall into four
buckets:

1. **Permissive** — MIT, BSD, Apache-2.0, zlib, 0BSD, Public Domain.
   **Safe to link from ziptastic.** Examples: `xz`, `libbsc`, `bzip3`,
   `brotli`, `zstd`, `lz4`, `zpaq`, `preflate`, `ryg_rans`, `fse`,
   `pcodec`, `bitshuffle`, `zlib-ng`, `libdeflate`, `zopfli`, `tamp`,
   `heatshrink`, `brieflz`, `snappy`, `density`, `lizard`.

2. **GPL / LGPL — do not link** — `paq8px` (GPL-2.0), `upx` (GPL-2.0),
   `advancecomp` (GPL-3.0), `packJPG` (LGPL-3.0), `lzo` (GPL/commercial
   dual), `7z PPMd` (verify per file, sometimes LGPL). These may be
   **spawned as processes** by heavenzip, but never linked into ziptastic.

3. **Dual** — some sources offer GPL or commercial. Default to the
   permissive branch if available. If not, treat as external-process-only.

4. **Unknown / verify** — some HCS sources are marked `verify` in
   `SOURCES.md`. Before linking any of them, copy their license text into
   `HCS/LICENSES/<id>-<spdx>.txt` and confirm the integration column in
   `SOURCES.md` is set correctly.

If you ever need a source that is not already in HCS, **stop and report
before adding it**. The user must approve new sources.

---

## 7. Where Everything Lives

Quick reference for the AI agent. Use this to find what you need without
hunting.

### `ziptastic/`

| You need | Path |
|---|---|
| The 12-phase build plan | `AGENT_PROMPT.md` |
| The joint parser to write | `src/engine/parser.cpp` + `include/chimera/parser.hpp` |
| The dual match finder to write | `src/engine/match_finder.cpp` |
| The per-block mode selector | `src/engine/mode_selector.cpp` |
| The rANS coder to write | `src/entropy/rans.cpp` |
| The Huffman coders to write | `src/entropy/huff_static.cpp`, `huff_adaptive.cpp` |
| LZMA2 fallback wrapper | `src/codecs/lzma2_fallback.cpp` |
| libbsc wrapper | `src/codecs/libbsc_wrap.cpp` |
| libzpaq wrapper (max-ratio only) | `src/codecs/zpaq_wrap.cpp` |
| Preflate wrapper | `src/precompressors/preflate_wrap.cpp` |
| Parmar wrapper | `src/precompressors/parmar_wrap.cpp` |
| Bitshuffle wrapper | `src/precompressors/bitshuffle_wrap.cpp` |
| Coarse block-hash index | `src/dedup/coarse_index.cpp` |
| Bitstream spec (write as you build) | `docs/FORMAT.md` |
| Benchmark log (update per phase) | `docs/BENCHMARKS.md` |
| Vendored sources (read-only) | `vendor/hcs/old/` |
| Source manifest (read-only) | `vendor/hcs/SOURCES.md` |

### `heavenzip/`

| You need | Path |
|---|---|
| The 8-phase build plan | `AGENT_PROMPT.md` |
| Master INI config | `config/syc.ini` |
| Per-type profiles | `config/profiles/*.ini` |
| INI parser to write | `src/orchestrator/ini.cpp` |
| File-type detector to write | `src/orchestrator/detector.cpp` |
| Subprocess dispatcher to write | `src/orchestrator/dispatcher.cpp` |
| Pipeline manifest writer | `src/orchestrator/pipeline.cpp` |
| Binaries (downloaded) | `bin/` |
| Binaries-download workflow | `.github/workflows/sync-binaries.yml` |

### `compfactory/`

| You need | Path |
|---|---|
| The 6-phase build plan | `AGENT_PROMPT.md` |
| Workspace sync (Windows) | `scripts/sync.ps1` |
| Workspace sync (macOS/Linux) | `scripts/sync.sh` |
| HCS → ziptastic propagation | `scripts/bump-hcs.ps1` |
| Build ziptastic from workspace | `scripts/build-all.ps1` |
| Cross-repo workflow trigger | `.github/workflows/dispatch-sync.yml` |
| Workspace documentation | `docs/WORKSPACE.md` |
| Developer workflow | `docs/WORKFLOW.md` |
| All cloned repos | `workspace/` (gitignored) |

### `HCS/`

| You need | Path |
|---|---|
| Sources (read-only) | `old/<id>/` |
| Machine-readable manifest | `SOURCES.md` |
| Human-readable catalog | `README.md` |
| License texts | `LICENSES/` |

---

## 8. Where to Start — The Critical Path

**Start in `ziptastic`.** That is the answer. Everything else follows.

### Why

- `HCS` is done. Only touch it to add sources.
- `compfactory` is done. Only touch it if a script breaks.
- `heavenzip` cannot work end-to-end until ziptastic produces a real
  release. Phases 0–3 can be verified with a placeholder binary, but the
  project's success hinges on ziptastic.
- `ziptastic` is the product. Every hour here compounds.

### The first-week plan

| Day | Repo | Phase | Milestone |
|---|---|---|---|
| 1 | `ziptastic` | Phase 0 | CMake + CLI skeleton + RAW round-trip |
| 2–3 | `ziptastic` | Phase 1 | LZMA2 fallback — **first "never worse than xz" milestone** |
| 4 | `ziptastic` | Phase 2 | Fine match finder (LZMA port) |
| 5 | `ziptastic` | Phase 3 | Joint parser (DP) |
| 6 | `ziptastic` | Phase 4 | rANS + context models — **first real CHCE backend** |
| 7 | `ziptastic` | Phase 6 | Coarse SREP index — **first time you beat xz on Deflate-heavy inputs** |

Once ziptastic Phase 6 passes its ratio gate on `silesia.zip`:

1. Tag `ziptastic v0.1.0` (triggers the release workflow).
2. Trigger heavenzip's `sync-binaries` workflow to pull the binaries.
3. Test heavenzip end-to-end.
4. Then consider the fifth repo (`awesome-compressor`).

### What to do first, right now

```powershell
cd C:\Users\Admin\ziptastic
notepad AGENT_PROMPT.md
