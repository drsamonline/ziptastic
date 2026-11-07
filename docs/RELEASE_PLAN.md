# Release plan: 1.0 first, speed after

Written 2026-11-04. The engine works. This is the order to make it *releasable*, then faster, then
broader — and the reasons the order is what it is.

The principle: **a solid archiver that never loses a byte beats a fast one that sometimes does.**
Every item below is either a correctness guarantee, a release surface, or a measured speed win.
Nothing here adds a format.

---

## The extension: `.dr`

**Recommendation: `.dr`.** Short, no collision in the compression space (`.zip`, `.7z`, `.xz`,
`.zst`, `.lz4`, `.br`, `.zpaq`, `.tzst`, `.lz` are all taken; `.dr` is not), and it reads as a
format name rather than a tool name — which matters because the container outlives the CLI.

Two rules go with it, and they matter more than the name:

1. **Detection is by magic, never by extension.** The file starts with `CHCE`; every decoder path
   already keys off that. A `.dr` renamed to `.bin` must still decode, and a `.dr` that is not a
   CHCE stream must be refused. The extension is a convention for humans and file managers.
2. **Split parts keep the HJSplit convention**: `archive.dr.001`, `archive.dr.002`. Already
   implemented, and it means `cat` and every existing split tool work unchanged.

Runners-up, for the record: `.zt` (reads as zstd, whose extension is `.zst`), `.chce` (accurate,
four characters, and nobody types it). If the name ever needs to signal the tool rather than the
format, `.ziptastic.dr` is available without changing anything.

**What to do about it**: register the magic in `scripts/` as a `file(1)` snippet, document the
extension in the README, and add a test that a `.dr` with the wrong extension still decodes.

---

## Phase 1 — release blockers (must happen before 1.0.0)

These are the things that make the difference between "a repo" and "a release".

1. **Version and identity.** `0.1.0-dev` becomes `1.0.0`; `--version` prints the format version
   alongside it (already does). One `VERSION` constant, referenced from CMake and the CLI.
2. **A README that states the claim and the caveat.** The 20–36%-better-than-`xz` number on a
   realistic tree, *and* the 0.3–0.6 MB/s speed, in the same paragraph. A release that hides the
   second number gets its first bug report about it.
3. **The extension and magic registration** (above).
4. **`ziptastic c` with no output path writes `input.dr`** rather than requiring `-o`. Small, and
   it is the first thing anyone types.
5. **A release archive**: source tarball, a Linux x86-64 binary, checksums, and the build recipe
   that actually works here (zig wrappers, target-by-target, depfile stripping) written down in
   `docs/BUILD.md`. That recipe cost this project many hours and lives only in a session log.
6. **A cold-start smoke test in CI**: build from scratch, run `ctest`, compress and decompress a
   sample. The suite is 32 tests and takes ~2 minutes; there is no excuse not to run it.
7. **Decide the licence statement.** `LICENSE` exists; the vendored libraries are third-party and
   their terms need one paragraph in the README, not a footnote.

**Exit criteria for 1.0.0**: a fresh clone builds on a clean machine with the documented recipe,
`ctest` is green, `ziptastic c file && ziptastic d file.dr` round-trips byte-exact, and the README
states ratio *and* speed.

## Phase 2 — solid ground (correctness work that must precede speed)

Speed work on a shaky base is wasted. These are small and each closes a real class of bug.

> **Done, 2026-11-04.** All four closed; `FINDINGS.md` §7 has the line for each.

1. ~~**A ZIP inside a plain tar is not reached**~~ — **fixed, and the cause was not the tar.** The
   members were streamed: zero sizes in the local headers, real ones in a data descriptor. The
   scan reads the central directory now. −42.8% on a streamed ZIP, −44.0% on a tar holding one.
2. ~~**`precomp_helps`' 2× rule is a predictor**~~ — **stated in the code**, with the bound named:
   the Tier-0 guard catches it under 2 MiB, nothing does above, and that is written where the line
   is rather than left implied.
3. ~~**The `layers3` free bits need a policy**~~ — `FORMAT.md`, "Policy: who may take a free layer
   bit": four conditions to allocate one, four changes that force a version bump instead.
4. ~~**A regression corpus**~~ — `scripts/regression-corpus.sh`, registered as `integration.corpus`:
   the streamed ZIP, the tar holding it, a small PNG (the transform must not make it bigger) and a
   near-duplicate tree, each asserting the property that broke, not just the round trip.

## Phase 3 — speed (only after phases 1 and 2)

Measured, in order of expected payoff. The selector profile already says where the time goes.

1. ~~**The candidate set costs what it costs.**~~ **Done, 2026-11-04.** Huffman is gated by a 64 KiB
   trial with no ratio change on six corpora (`FINDINGS.md` §3e); the LZMA2 fallback is gated by a
   64 KiB trial with a 20% margin, worth a third of the run on a mixed tree (§3f). BWT still costs
   0.6% and wins 2.6%, so it stays ungated.
2. ~~**Block size vs ratio**, measured rather than assumed~~ — **swept, 2026-11-04.** 4 MiB stays:
   it is 4.3–9.3% smaller than 256 KiB on homogeneous content and within 0.6% of the best on every
   corpus. `FINDINGS.md` §3d has the table, and the one corpus where 1 MiB wins, which is the
   heterogeneous one — the selector's freedom to pick a different backend per block.
3. ~~**The base64 gate's 2.7× cost.**~~ **Partly done, 2026-11-04.** A 64 KiB first pass decides when
   the two sides differ by 20% — three times the sample's known error — and hands everything closer
   to the existing 256 KiB trial: 1.76 s → 1.08 s where the answer is obvious, no change where it is
   a tie. `FINDINGS.md` §3g.
4. **Threads.** Archive mode is sequential by design for ratio; the measurement that would justify
   more parallelism is a per-block time split, not a guess. **Blocked on hardware, 2026-11-04**: the
   box these numbers come from reports two cores and delivers one core of throughput, so a per-block
   time split here would measure oversubscription. `FINDINGS.md` §0.

**Non-goals for speed**: competing with `zstd -3` on wall clock. This engine's claim is ratio at a
speed that is tolerable for archival work, not interactive use.

## Phase 3b — addressable blocks (one format change, four features)

> **Done, 2026-11-04 — format v8.** `docs/FORMAT_V8.md` is the record, `FINDINGS.md` §3c has the
> numbers. The offset table is in the trailer; `read_index`, `decode_blocks`, `ziptastic info`,
> `d --block N` and `d --from-block N` read it. The four features below are the ones it was built
> for, and the fourth (hatching) remains heavenzip's to build on top. What it does **not** do is
> make archive mode addressable: that mode is sequential by construction, and a partial decode of
> it is refused rather than guessed at.

`docs/HATCHING.md` records what was taken from HatchZip: not its shrinking extraction, which is an
extractor's job with an extractor's risks, but the property that makes it possible — **a block
offset table**.

Parallel-mode blocks are already self-contained and carry their own SHA-256; nothing can find them
without walking from the front. An offset table in the trailer (format **v8**) would give:

- hatch-style extraction from the last block backwards, truncating as it goes;
- random access — decode one block without reading the others;
- resumable decode — restart at a block boundary after a crash;
- parallel decode without a sequential scan first.

This is the first thing that reads an offset index; the earlier lexora note proposed one and it was
not built because nothing did. Cost: one trailer field per block, and a decoder that can start at
an offset.

## Phase 4 — reach (transforms, only when something asks for them)

New Layer 0 transforms need a measured win on *real* content and a free bit. The list of what has
been measured and rejected (`FINDINGS.md` §5) is longer than the list of what remains, and that is
healthy: the engine's advantage is already concentrated in a handful of formats.

**Closed on 2026-11-07**: SWF is dropped — Flash is dead and there was nothing to win. PDF is *not* a
transform opportunity in the shape it was assumed: on a real 99-page scanned document the engine gets
85.5%, `precomp` correctly declines, and the gap is in **1-bit imagery** — which is a JBIG2 or CCITT
G4 transform, a different and much larger piece of work than anything in this phase.

**Something asks, and it has been measured: the opaque streams.** `.tar.zst`, `.br` and `.xz` are
invisible to the engine, and a `.tar.zst` costs **22.7% more through the engine than the tar itself**.
Seeing inside them is worth **−18.5% (zstd), −14.7% (brotli), −16.9% (xz)** on the measured fixture,
and the recipe is a few bytes in the easy case because re-encoding with the same encoder reproduced
all three streams byte for byte. `FINDINGS.md` §3h has the numbers, the free-bit arithmetic against
the policy in `FORMAT.md`, and the part that is not established: recovering the producing settings
from a stream whose encoder version and level are unknown, which is `preflate`'s actual difficulty.

Order of work if this is picked up: recipe recovery first, because its cost decides whether the
transform ships at all — **answered 2026-11-04**: a full search over settings costs 2.5–4× the whole
compression, a two-entry shortlist costs 0.5 s for zstd and 1.2 s for xz. A ten-stream corpus from
different producer shapes then reordered the work again:

1. ~~**`.xz` first.**~~ **Done, 2026-11-07.** `xz_wrap.cpp`, `kLayer3Xz`, `unit.xz`: a `.tar.xz` of
   156,904 bytes now archives to **130,449** against 130,435 for the tar itself, −16.9% on the
   measured fixture, byte-exact. The recipe is dictionary size, integrity type, a preset byte and an
   `extreme` bit; finding it is one or two trial encodes. `FINDINGS.md` §3h.
   Its dictionary size is in the block header, so recovery is a *read* and not a
   search — and reading it reproduced every stream tested byte-exactly, including one a shortlist
   missed. `liblzma` is already vendored. **Proved in-tree 2026-11-07**: `tests/benchmark/xz_probe.cpp`
   decodes, reads the dictionary size from the header, and re-encodes byte-identically at presets 0, 6
   and 9 — including streams written by liblzma 5.4.1 and reproduced by the tree's 5.8.4. The recipe
   is a dictionary size plus one `extreme` bit. `FINDINGS.md` §3h.
2. **`.zst` second.** Needs a (level, framing) shortlist of four, because a stream written to a pipe
   omits the content size and no seekable encode can reproduce it. Needs a decoder the tree lacks.
3. **`.br` last.** Quality 11 costs 2.5 s to confirm and the header does not carry the quality.

Still missing before anyone builds it: a stream from a *different encoder version*. Everything tested
came from the same libraries, and a zstd 1.4 stream may not be reproducible by a zstd 1.5 encoder at
the same level. That needs real-world files.

---

## What "done" means

Not "no more transforms". **Done is: a release someone else can build, run and trust, whose
documented numbers are true, and whose failure modes are named.** Phases 1 and 2 reach that. Phases
3 and 4 are improvements to a working thing, and can stop whenever they stop paying.
