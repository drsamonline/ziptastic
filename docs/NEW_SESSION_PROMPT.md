# Paste this to start a new session

---

You are continuing work on **ziptastic** (GitHub `drsamonline/ziptastic`), a lossless compressor by
Dr Sohil Momin. **Read `docs/FINDINGS.md` before anything else — it is the authority.** Every current
number, the audit, the rejected ideas and the open items are there; where it and any other document
disagree, `FINDINGS.md` wins. The per-item documents are history and several were revised in place.

## Where the project is

**1.0.0 shipped at format v7. Current tree: 1.1.0, format v8, 33/33 suites green, 53 CLI
assertions green.** A working archiver: `ziptastic c file` writes `file.dr`, `ziptastic d file.dr`
gives it back byte-exact. On a realistic mixed game-asset tree it is **20–36% smaller than `xz -9e`**;
at 0.3–0.6 MB/s it is much slower, and that trade is deliberate. It does not win where no format
transform fires — the cooked-ISO and floppy rows in `FINDINGS.md` show it level with `xz`.

**v8 archives are not readable by 1.0.0**, and 1.0.0's v7 streams are refused by this tree with
`bad-version`. That is the project's usual clean break; there are no v7 archives in the wild to
strand.

Read next, in this order: `docs/RELEASE_PLAN.md` (the order of work and why), `docs/BUILD.md` (the
recipe, the depfile trap that costs a debugging session, and the pip-only toolchain that works when
the sandbox has no compiler), `docs/AUDIT_FINDINGS.md` (eight findings, six fixed), `docs/HATCHING.md`
(the layer split decided on 2026-11-04), `docs/FORMAT_V8.md` (what was built last and why it is
shaped that way).

## What was just done: Phase 3b, the block offset table

**Done.** Format v8 puts one absolute u64 per block in the trailer, behind a `ZTOX` tail marker, so
a reader can find the table from the end of the stream without walking the blocks in front of it.
`docs/FORMAT_V8.md` is the design record; `FINDINGS.md` §3c has the measurements (0.76% of a
1.38 MB archive read to reach any one of 225 blocks; one block decoded in about 1/100th of a full
decode; the table costs 0.01–0.13% of an archive and **1.7% of a small single-block one**).

Three things it deliberately does not do, so nobody rebuilds them: archive mode is still
sequential and `--block` on it is a refusal; a transformed stream cannot be sliced (the inverse
chain needs everything); and the hatch *pipeline* — fit check, journal, truncate-as-you-go — is
heavenzip's, calling HatchZip for foreign archives.

## Phase 2 is closed

All four items, 2026-11-04. The one that mattered was not the tar: a ZIP written to a pipe
leaves zero sizes in its local headers and puts them in a data descriptor, so `scan_zip` skipped
every member — and a tar around it changed nothing. −42.8% on a streamed ZIP, −44.0% on a tar
holding one, and the corpus in `scripts/regression-corpus.sh` now asserts it.

## The next piece of work: Phase 3, speed

`RELEASE_PLAN.md` §Phase 3. Started, barely: the block digest check moved into the decode workers,
where it is strictly less serial work — and it could not be measured to help, because this machine
cannot resolve it. **Two cores, ±40 ms per run on an 800 ms decode.** Anything under ~5% needs many
more runs or a quieter box before it is a claim; the earlier "4–6% for index-driven decode" was a
benchmark artefact (order bias, now fixed in `index_cost`), and `FINDINGS.md` §3c says so.

The measured levers are still: not running candidates that cannot win (a sample trial per family), a
block-size sweep (256 KiB is nobody's decision, 4 MiB is the default and nothing has swept it), the
base64 gate's 2.7× cost, and a per-block time split before any more threads.

## How this project works, and what it keeps relearning

- **Measure before building, and measure on real content.** Every figure this project ever took from
  a generated fixture turned out optimistic — BMP's −99% was −37% on photographs, PNG's −79% nested
  was −0.3%. The fixtures that held up were real files.
- **Verify every transform by rebuilding its input and byte-comparing.** That is what makes a wrong
  transform cost ratio instead of data.
- **A green suite proves nothing about an untested path.** The audit found three serious bugs in a
  suite that was green throughout; the fuzz test had been fuzzing nothing because it used default
  options. Check what a test actually exercises before trusting it.
- **A green suite proves nothing about an untested path** — and a round trip proves nothing about
  whether a transform fired. `scripts/regression-corpus.sh` exists because every one of those bugs
  round-tripped perfectly while the archive sat at input size.
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
