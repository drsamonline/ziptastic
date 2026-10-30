# Holistic Compression Sources (HCS)

Canonical, offline, vendored source vault for the **CHCE** engine
(Chimera Hybrid Compression Engine) and all CHIMERA-family projects
(`heavenzip`, `ziptastic`, `compfactory`).

Every third-party compressor, preprocessor, recompressor, and
entropy-coder reference used by CHCE lives in [`/old`](./old), pinned at a
known-good revision. Consumers pull this repo with `git subtree` and build
from the pinned tree.

**Design constraints (non-negotiable):**

1. **Offline-only.** No neural networks, no model files, no GPU runtimes.
   Context-mixing (CM) compressors like `zpaq`, `paq8px`, `lpaq` are pure
   algorithms — they are **not AI** and are fully offline.
2. **Single source of truth.** No submodules, no nested `.git` directories.
   All sources are vendored flat.
3. **License-audited.** Every source is listed with its license in
   `SOURCES.md` and full text in `LICENSES/`.

---

## Integration Model

| Consumer | How it uses HCS |
|---|---|
| `ziptastic` (native engine) | `git subtree` of `HCS` into `vendor/` |
| `heavenzip` (orchestrator) | Consumes **prebuilt binaries** released by `ziptastic` — does not build from `/old` |
| `compfactory` (workshop) | Full checkout; edits propagate back to HCS via PR |

> **Rule:** `heavenzip` must never contain algorithm code; `ziptastic` must
> never shell out to external executables. HCS exists to feed `ziptastic`.

---

## Repository Layout
HCS/
├── README.md # this file
├── SOURCES.md # machine-readable manifest (id, url, pin, license)
├── LICENSES/ # one .txt per source (full license text)
├── old/ # vendored sources (the vault)
│ ├── xz/
│ ├── zpaq/
│ ├── srep/
│ └── ...
└── (future)
└── chimera/ # optional: CHCE glue code that consumes /old

---

## Source Catalog

Grouped by role in a CHCE pipeline.

### 🧮 Entropy / Final-Stage Coders

| Source | Role | License | Pin |
|---|---|---|---|
| `xz` | LZMA2 reference implementation | 0BSD / LGPL-2.1 | `TBD` |
| `lzma` | LZMA SDK (raw codec) | Public Domain | `TBD` |
| `7z` | 7-Zip SDK — PPMd + LZMA | Public Domain | `TBD` |
| `brotli` | Static dictionary + context-modeled coder | MIT | `TBD` |
| `fse` | FiniteStateEntropy + Huff0 (rANS/Huffman reference) | BSD-3 | `TBD` |
| `ryg_rans` | Minimal rANS reference (Charles Bloom) | Public Domain | `TBD` |

### 🔗 LZ / Match-Finder Backends

| Source | Role | License | Pin |
|---|---|---|---|
| `srep` | Long-range block-hash matcher | Public Domain | `TBD` |
| `zstd` | Speed/ratio spectrum backend | BSD-3 / GPLv2 | `TBD` |
| `lz4` | Fast baseline backend | BSD-2 | `TBD` |
| `lzham` | LZMA-class alternative | MIT | `TBD` |
| `freearc-c` | LZMA, PPMd, Tornado, GRZip bundle | varies | `TBD` |
| `freearc-main` | FreeArc (original) | mixed | `TBD` |
| `FA-master` | FreeArc Next (reference only — incomplete C++) | mixed | `TBD` |

### 🧠 Context-Mixing Backends (Ratio Ceiling — pure algorithm)

| Source | Role | License | Pin |
|---|---|---|---|
| `zpaq` | ZPAQ CM engine (`libzpaq`) | Public Domain | `TBD` |
| `zpaqfranz` | Maintained ZPAQ fork, robust CLI | PD / GPL | `TBD` |
| `paq8px` | Highest-ratio non-neural CM | GPL-2.0 | `TBD` |
| `LPAQ9` | Lightweight PAQ variant | Public Domain | `TBD` |
| `omnizip-zpaq` | Rust CM (`omnizip-rs`) — reference | verify | `TBD` |

### 🔄 BWT Backends

| Source | Role | License | Pin |
|---|---|---|---|
| `libbsc` | BWT + LCP + ANS | Apache-2.0 | `TBD` |
| `bzip3` | Modern BWT successor to bzip2 | Apache-2.0 | `TBD` |

### 🔢 Numeric / Structured Data

| Source | Role | License | Pin |
|---|---|---|---|
| `pcodec` | Quantile-based numeric compressor | Apache-2.0 | `TBD` |
| `bitshuffle` | Bit-transpose for typed arrays | MIT | `TBD` |

### ⚡ Fast / Asymmetric Codecs

| Source | Role | License | Pin |
|---|---|---|---|
| `zxc` | Ultra-fast decode, better than LZ4 | MIT (verify) | `TBD` |
| `density` | Ultra-fast LZ, near-memory-bandwidth decode | BSD-2 | `TBD` |
| `csc` | LZMA-inspired fast LZ codec | MIT (verify) | `TBD` |
| `lizard` | LZ4 fork with moderate-ratio tier | BSD-2 | `TBD` |
| `brieflz` | Tiny LZ77 for constrained targets | zlib | `TBD` |
| `snappy` | Google fast codec (benchmark baseline) | BSD-3 | `TBD` |
| `lzo` | Classic fast codec (kernel/embedded) | GPL / commercial | `TBD` |

### 🧹 Preprocessors

| Source | Role | License | Pin |
|---|---|---|---|
| `parmar` | Subword tokenization for text | verify | `TBD` |
| `shoco` | Short-string compressor | MIT | `TBD` |
| `smaz` | Very-short-string compressor | BSD-3 | `TBD` |

### 🖼️ Recompressors (Format-Aware)

| Source | Role | License | Pin |
|---|---|---|---|
| `preflate` | DEFLATE recompressor (C++) | Apache-2.0 | `TBD` |
| `preflate-rs` | DEFLATE recompressor (Rust) | Apache-2.0 | `TBD` |
| `precomp-cpp` | Legacy Precomp (reference) | Apache-2.0 | `TBD` |
| `packJPG` | JPEG recompressor ⚠ **LGPL-3** | LGPL-3 | `TBD` |
| `aqueous` | Recompression archive (~20 format families) | MIT-0 / PD | `TBD` |
| `rhefo` | MIME-aware file optimizer | MIT | `TBD` |
| `zopfli` | Iterative Deflate optimizer | Apache-2.0 | `TBD` |
| `advancecomp` | Archive recompressor (gzip/PNG/ZIP) | GPL-3 | `TBD` |

### 🧱 Fast Deflate (Encoder Path)

| Source | Role | License | Pin |
|---|---|---|---|
| `zlib-ng` | Fast, standards-compliant Deflate | zlib | `TBD` |
| `libdeflate` | Fast Deflate (encoder + decoder) | MIT | `TBD` |

### 📦 Executable Packers

| Source | Role | License | Pin |
|---|---|---|---|
| `upx` | Classic executable packer | GPL-2.0 | `TBD` |
| `zelf` | Modern ELF packer (x86_64, AArch64) | verify | `TBD` |

### 🪶 Embedded / Low-Memory

| Source | Role | License | Pin |
|---|---|---|---|
| `tamp` | Low-memory DEFLATE-inspired | MIT | `TBD` |
| `heatshrink` | Minimal LZSS for RT/embedded | ISC | `TBD` |

### 📚 Discovery / Reference Lists

| Source | Role | License | Pin |
|---|---|---|---|
| `awesome-compopt` | Compression tool catalog | CC / MIT | `TBD` |
| `awesome-cpp` | C++ library list (compression section) | CC / MIT | `TBD` |

### 🧪 Additional / Experimental

| Source | Notes | License | Pin |
|---|---|---|---|
| `BARF` | Experimental backend | verify | `TBD` |
| `BBB` | Experimental backend | verify | `TBD` |
| `FLZP` | Experimental LZ family | verify | `TBD` |
| `FPAQF2` | PAQ-family derivative | verify | `TBD` |
| `SR3A` | Experimental | verify | `TBD` |

> "verify" = license text needs to be copied into `LICENSES/` before
> integration. `TBD` in the pin column is replaced by a commit SHA the
> first time the source is built against.

---

## ⚠️ License Warnings

- **`packJPG` is LGPL-3.** If CHCE is closed-source, do **not link** it.
  Treat it as an **external process** invoked by `heavenzip`, or drop JPEG
  recompression for now.
- **`advancecomp` is GPL-3.** Same rule: external process, not linked.
- **`upx` is GPL-2.0.** Same rule.
- **`lzo` is GPL / commercial dual.** Do not link unless CHCE is GPL.
- **`paq8px` is GPL-2.0.** Treat as an **external process** in
  `heavenzip` for the `--max-ratio` tier, not a linked library.

All other listed sources are permissive (MIT, BSD, Apache-2.0, zlib, PD,
0BSD) and safe to link into a closed-source CHCE core.

---

## 🏆 The Offline Ratio Ladder

CHCE selects a tier per block based on the joint parser's cost model.
This is the target performance envelope across the sources in `/old`:

| Tier | Backend(s) | Ratio vs xz | Speed vs xz |
|---|---|---|---|
| **Max ratio** | `paq8px` / `LPAQ9` | 0.70–0.80× | 0.001× |
| **High ratio** | `zpaqfranz -m5` / `zpaq` | 0.80–0.85× | 0.01× |
| **Strong** | `bzip3` / `libbsc` / PPMd (`7z`) | 0.90–0.98× | 0.5–2× |
| **Baseline** | `xz` (LZMA2) | 1.00× | 1.00× |
| **Balanced** | `zxc` / `zstd -19` / `brotli -q11` | 1.05–1.15× | 3–10× |
| **Fast** | `density` / `lz4` / `csc` / `lizard` | 1.3–1.5× | 20–50× |
| **Ultra-fast** | `heatshrink` / `brieflz` / `snappy` | 2× | 100×+ |

Plus these orthogonal transforms that operate **before** the entropy tier:

- **Semantic recompression** — `preflate`, `preflate-rs`, `precomp-cpp`,
  `aqueous`, `rhefo` (DEFLATE, JPEG, PNG, PDF, PE, SQLite, ~20 formats)
- **Text tokenization** — `parmar` (+7–9.6 % on LZMA2 over raw text)
- **Numeric bit-transpose** — `bitshuffle`, `pcodec` (2–10× on typed arrays)
- **Short-string coding** — `shoco`, `smaz`
- **Long-range dedup** — `srep` (matches beyond LZMA's 64 MB window)
- **Executable packing** — `upx`, `zelf`

---

## Bumping a Source

1. `cd old/<source>` and check out the desired revision.
2. Update the `pin` in `SOURCES.md`.
3. Copy the license text into `LICENSES/<source>-<license>.txt`.
4. Regenerate any integration tests in `ziptastic` that consume this source.
5. Open a PR titled `bump: <source> → <new-sha>`.

---

## Adding a New Source

1. `git clone --depth=1 <url> old/<id>` **inside this repo**.
2. `rm -rf old/<id>/.git` — **critical, do not skip**.
3. Add a row to `SOURCES.md`.
4. Add the license text to `LICENSES/`.
5. Add a row to the appropriate section of this README.
6. Commit: `add: <id> (<role>)`.

---

## Quick Verification

To check that `/old` contains no nested `.git` folders (the most common
mistake when vendoring):

```bash
find old -name .git -type d
# should print nothing
