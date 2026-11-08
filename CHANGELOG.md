# Changelog

Every release, and what actually changed. Written from the commits rather than from memory, because
release notes reconstructed at the end of a session are how a project ends up with a version that
claims something it did not ship.

The authority for any number quoted here is [`docs/FINDINGS.md`](docs/FINDINGS.md). This file is the
short version: what changed, and what it cost.

---

## 1.4.1 — 2026-11-08

### Fixed

- **The compressor had no inflate cap of its own.** The three container transforms capped inflation
  at a flat 4 GiB and the compressor allocated up to it, while the decompressor has always used a
  relative cap. A 28,971-byte `.zst` inflating to 900 MB took the old build down; it now declines and
  stores the stream opaquely, peaking at ~524 MB instead of asking for 4 GiB. The cap is
  `max(input × 4096, 256 MiB)` in all three — the decoder's policy with a floor, so a 6 KB stream
  inflating to 200 MB (a tar of zeros) still transforms. (`FINDINGS.md` §6c)
- **Base64 offered itself on a binary scanned PDF.** The detector accepted any single run of 32
  alphabet bytes, and a PDF dictionary supplies one; on a 7.4 MB scan that produced `layers 0x10` and
  a wasted trial. It now requires the alphabet to cover at least 1/64 of the file, so a real base64
  file still fires and a 32-byte accident in 7 MB does not.
- **A short encode now reports itself.** `brotli_wrap` and `zstd_wrap` relied on the re-encode
  verifier to notice a stream that never reached FINISH. Both now say so where the cause is known.
- **`expansion_cap` is saturating rather than wrapping.** It multiplies an attacker-supplied
  `true_size`; the old wrap rejected by accident, and now it cannot wrap at all.

### Added

- **A marker convention.** `TODO(name-date): what and why`, documented in `BUILD.md`, and the six
  `catch (...)` blocks that swallow everything are marked with it. There were zero markers in 12,460
  lines before today, which is why the audit found three things a grep should have.

## 1.4.0 — 2026-11-08

### Added

- **A `.zst` transform**, the last of the three opaque containers and the largest gap of them. On a
  542,720-byte tar: a level-3 `.tar.zst` went from 196,091 bytes through the engine to **136,683**
  (−30.3%) and a level-19 one from 167,730 to the same 136,683 (−18.5%), including the **piped**
  framing, which needed the encoder to feed its input in 128 KiB chunks to match a writer's block
  boundaries. Vendors `zstd` (BSD-3-Clause, trimmed, `ZSTD_DISABLE_ASM`). `unit.zstd` in the suite;
  37 suites now.

- **A `.br` transform.** Brotli has no magic number and does not record its quality, so the
  transform decodes, guesses the producing settings from a shortlist of six, and keeps the guess only
  if re-encoding reproduces the stream byte for byte. On a 542,720-byte tar: a quality-11 `.tar.br`
  went from 159,975 bytes through the engine to **136,299** (−14.8%), and a quality-1 one from
  217,005 to the same 136,299 (−37.2%). `unit.brotli` in the suite; 36 suites now.

## 1.3.0 — 2026-11-07

### Added

- **An `.xz` transform.** A `.tar.xz` used to be opaque: the archive came out slightly larger than
  the stream. It is now inflated and the stream is rebuilt from a recipe read out of its own block
  header — dictionary size, integrity type, a preset byte, one `extreme` bit — with nothing of the
  original stored. On a 522,240-byte tar: 156,904 bytes before, **130,449** after, against 130,435
  for the tar itself. Byte-exact, `layers3` reads `0x02`, and it costs one inflate plus one or two
  trial encodes. (`FINDINGS.md` §3h)
- **Nested transforms inside an `.xz`.** The `.xz` stage was not connected to the seven nested gates,
  which still tested for `precomp || gzip`, so a `.tar.xz` holding a CSV or a ZIP was inflated and
  then left alone. Fixed, with a test that fails without the fix.
- **`unknown-transform`.** A stream that uses a Layer 0 bit this build does not know now says so.
  Before, it was `bad-flags` — "invalid header flags" — which reads as corruption in a file that is
  perfectly intact. Confirmed with a 1.2.0 binary against a stream carrying the `.xz` bit. The format
  version stays v8, because the format did not change; the reader's knowledge did. (`FORMAT.md`)

- **The first benchmark row anyone else can check.** Silesia, 211,938,580 bytes: ziptastic `-9` gives
  **43,271,420 (20.42%)** against an `xz -9e` equivalent built from the vendored liblzma at
  **48,456,004 (22.86%)** — 10.7% smaller. `scripts/silesia.sh` regenerates it. (`FINDINGS.md` §6a)

### Measured, not built

- **bzip2**: a `.tar.bz2` costs 9.7% more through the engine than the tar does, and its level is in
  its own header, so the recipe would be one byte — but `libbz2` is not vendored and `.tar.bz2` is
  rarer than `.tar.xz`.
- **zstd and brotli** stay unbuilt: a shortlist must be a (level, framing) pair because a stream
  written to a pipe omits its content size, and `.zst` needs a decoder the tree does not have.

## 1.2.0 — 2026-11-04

**The same archives, less time.** No archive size changed in this release. Format stays **v8**, so
1.1.0 and 1.2.0 read each other's archives.

### Faster

- **The Huffman candidate is gated by a measurement.** It cost 18% of a text run and produced
  byte-identical output there, while winning 0.73% on a seven-symbol file — so it could be neither
  left on nor turned off. A 64 KiB trial now decides, and no archive size changed on six corpora.
  (`FINDINGS.md` §3e)
- **The LZMA2 fallback is gated with a 20% margin.** It cost 43% of a run on a mixed asset tree and
  won nothing there: 6.6 s → 4.1 s on 1.6 MB, no archive change. The margin is 20% because 5%
  skipped exactly the same corpora and saved nothing more — the safety is free. (`§3f`)
- **The base64 gate got a decisive first pass.** Its full trial costs 2.1–2.3× the whole run; a
  64 KiB pass now settles the obvious cases, 1.76 s → 1.08 s, and leaves ties to the expensive pass.
  (`§3g`)
- **Block digests are checked in the decode workers** instead of after the join. Strictly less serial
  work; too small to measure on this machine, which is why it is described that way.

### Measured, and not built

- **Block size swept at last** (64 KiB–16 MiB, four corpora): 4 MiB stays. 4.3–9.3% smaller than
  256 KiB on homogeneous content, within 0.6% of the best everywhere. (`§3d`)
- **An adaptive block size rejected**, with its mechanism measured: removing BWT widens the 0.6% gap
  to 6.7%, so it is not the selector's freedom alone, and the trial would cost a second encode of
  every block. (`§3d`)
- **Phase 4 opened**: a `.tar.zst` costs 22.7% more through the engine than the tar itself; seeing
  inside zstd/brotli/xz is worth −18.5/−14.7/−16.9%. Recipe recovery settled as a shortlist rather
  than a search, then reordered by a ten-stream corpus: xz needs a header read, zstd needs four
  candidates because a pipe changes the frame. (`§3h`)

### Known limits, stated

- **`--parallel` and `-T` are untested.** The development box reports two cores and delivers one
  core of throughput; every thread claim in `FINDINGS.md` was measured there. (`§0`)
- The LZMA2 fallback's guarantee is now conditional on its 20% trial, and `README.md` says so.

### Checks

34/34 suites, 53 CLI assertions, `scripts/regression-corpus.sh` green. Binary:
`ziptastic-1.2.0-linux-x86_64`, glibc 2.17+, SHA-256 in `SHA256SUMS`.

---

## 1.1.0 — 2026-11-04

**Format v8: addressable blocks.** No header field changed; the trailer grew.

### Added

- **A block offset table in the trailer** — one absolute `u64` per block behind a `ZTOX` tail marker,
  so a reader finds the table by arithmetic from the end of the stream instead of walking the blocks.
- **`ziptastic info`** — header, block table, every block's offset, size and mode, verified against
  the manifest before anything is printed.
- **`ziptastic d --block N`** and **`--from-block N`** — one block on its own, and a decode that
  resumes at a block boundary.
- **`decode_blocks` / `decode_blocks_parallel`** in the library, for a caller holding an index rather
  than a stream.
- **Streamed ZIPs are reached.** A ZIP written to a pipe puts its member sizes in a data descriptor,
  and the scan read only local headers, so every member was skipped: 320,424 bytes → **183,286**
  (−42.8%), and the same ZIP inside a tar 321,197 → **183,454** (−44.0%). This had been recorded as
  "a ZIP inside a plain tar is not reached"; the tar was never the obstacle. (`§3b`)
- **A regression corpus** (`scripts/regression-corpus.sh`) that asserts the property each historical
  bug broke, not merely that the round trip works.

### Changed

- **Archive mode is still sequential**, and `--block` on one is a refusal by design. A partial decode
  of a transformed stream is refused too: the inverse chain needs the whole stream.
- **v7 streams are refused** with `bad-version`. 1.0.0 cannot read v8 archives, and this cannot read
  v7 ones.

### Cost

The table is `8 × blocks + 4` bytes: 0.13% of a 225-block archive, and **1.7% of a 687-byte one** —
recorded as a real cost rather than rounded away. (`§3c`)

---

## 1.0.0 — 2026-11-04

First release: format v7, nine encoders per block, eighteen Layer 0 transforms, 32 suites. On a
realistic mixed asset tree, 20–36% smaller than `xz -9e` at 0.3–0.6 MB/s. `FINDINGS.md` §3 has the
table and the rows where it is level with `xz` rather than ahead.
