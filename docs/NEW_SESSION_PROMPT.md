# Paste this to start a new session

---

You are continuing work on **ziptastic** (GitHub `drsamonline/ziptastic`), a lossless compressor by
Dr Sohil Momin. **Read `docs/FINDINGS.md` before anything else — it is the authority.** Every current
number, the audit, the rejected ideas and the open items are there; where it and any other document
disagree, `FINDINGS.md` wins. The per-item documents are history and several were revised in place.

## Where the project is

**Released at 1.0.0, format v7, 32/32 suites green, 39 CLI assertions green.** A working archiver:
`ziptastic c file` writes `file.dr`, `ziptastic d file.dr` gives it back byte-exact. On a realistic
mixed game-asset tree it is **20–36% smaller than `xz -9e`**; at 0.3–0.6 MB/s it is much slower, and
that trade is deliberate. It does not win where no format transform fires — the cooked-ISO and
floppy rows in `FINDINGS.md` show it level with `xz`.

Read next, in this order: `docs/RELEASE_PLAN.md` (the order of work and why), `docs/BUILD.md` (the
recipe, and the depfile trap that costs a debugging session), `docs/AUDIT_FINDINGS.md` (eight
findings, six fixed), `docs/HATCHING.md` (the layer split decided on 2026-11-04).

## The next piece of work: Phase 3b, the block offset table

`RELEASE_PLAN.md` §Phase 3b. Format **v8**: a per-block offset table in the trailer. Parallel-mode
blocks are already self-contained and carry their own SHA-256, so they are already independently
decodable — what is missing is any way to *find* one without walking from the front.

One change buys four features: hatch-style extraction from the last block backwards, random access
to a single block, resumable decode after a crash, and parallel decode without a sequential scan.
It is also the prerequisite for heavenzip hatching a `.dr` at all. Do this before any speed work:
three of the four features are speed and robustness.

## How this project works, and what it keeps relearning

- **Measure before building, and measure on real content.** Every figure this project ever took from
  a generated fixture turned out optimistic — BMP's −99% was −37% on photographs, PNG's −79% nested
  was −0.3%. The fixtures that held up were real files.
- **Verify every transform by rebuilding its input and byte-comparing.** That is what makes a wrong
  transform cost ratio instead of data.
- **A green suite proves nothing about an untested path.** The audit found three serious bugs in a
  suite that was green throughout; the fuzz test had been fuzzing nothing because it used default
  options. Check what a test actually exercises before trusting it.
- **Write the negative results down.** `FINDINGS.md` §5 exists so nobody rebuilds LZ4
  precompression, zip-store unpacking, adaptive Huffman or the precomp-gate move.
- **Never delete a user's data.** ziptastic does not remove its input; destructive decisions belong
  to the layer that was told to extract.

## Working notes

- The build takes several minutes; `docs/BUILD.md` has the vendor-by-vendor recipe and the reason
  each step exists. With depfiles stripped, a header change does **not** trigger rebuilds — touch
  `src/` and `tests/` `.cpp` files after touching anything in `include/`.
- Commits go through the GitHub API (the repo is on `main`); verify the tree hash after each push.
- The sandbox loses the build between sessions: budget the first few minutes for a clone and a
  configure.
- The engine's own docs use British spelling and no marketing voice. Keep that.
