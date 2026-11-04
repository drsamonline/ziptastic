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

1. **A ZIP inside a plain tar is not reached** (`FINDINGS.md` §3b). `scan_zip` misses it because
   of the 512-byte tar headers. Small fix, real gap for game trees.
2. **`precomp_helps`' 2× rule is a predictor.** It is the only place a predictor decides alone.
   Either make it measure, or state in the code why it is safe — but not neither.
3. **The `layers3` free bits need a policy**: what may take one, and what triggers format v8.
   Otherwise the next transform is decided by whoever is holding the keyboard.
4. **A regression corpus**: the fixtures that caught real bugs (the `.tar.gz` regression, the
   1.2 KB PNG, the near-duplicate tree) checked in as a script, so `ctest` covers them.

## Phase 3 — speed (only after phases 1 and 2)

Measured, in order of expected payoff. The selector profile already says where the time goes.

1. **The candidate set costs what it costs.** On 1 MB of prose: Huffman was 18% for nothing (now
   gated off by default), the LZMA2 fallback is 34% and is a guarantee, and BWT costs 0.6% while
   winning 2.6%. The remaining lever is **not running candidates that cannot win** — a sample
   trial per family, which is what `bcj_helps` and `precomp_helps` already do.
2. **Block size vs ratio**, measured rather than assumed: 256 KiB is the default and nothing has
   swept it.
3. **The base64 gate's 2.7× cost.** Three cheapenings have already failed; the honest option is a
   cheaper *trial* (fewer blocks, one backend) with the current gate as the fallback when the
   trial is close.
4. **Threads.** Archive mode is sequential by design for ratio; the measurement that would justify
   more parallelism is a per-block time split, not a guess.

**Non-goals for speed**: competing with `zstd -3` on wall clock. This engine's claim is ratio at a
speed that is tolerable for archival work, not interactive use.

## Phase 4 — reach (transforms, only when something asks for them)

New Layer 0 transforms need a measured win on *real* content and a free bit. The list of what has
been measured and rejected (`FINDINGS.md` §5) is longer than the list of what remains, and that is
healthy: the engine's advantage is already concentrated in a handful of formats.

---

## What "done" means

Not "no more transforms". **Done is: a release someone else can build, run and trust, whose
documented numbers are true, and whose failure modes are named.** Phases 1 and 2 reach that. Phases
3 and 4 are improvements to a working thing, and can stop whenever they stop paying.
