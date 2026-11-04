# What to take from HatchZip, and what not to

Written 2026-11-04, after reading `xevrion/hatchzip` (`docs/how-it-works.md` and the engine notes).

HatchZip is an **extractor** that solves a disk-space problem: a 180 GB archive that unpacks to
200 GB normally needs 380 GB free, because the archive and its output exist together. HatchZip
extracts from the **end**, truncating the archive after each unit is safely on disk, so the run
needs roughly the output size plus one unit.

It is a good piece of engineering and its central idea is worth taking. Its product is not ours.

## What ziptastic is, in this comparison

ziptastic compresses **a stream**, not a container of files. Its output is one CHCE stream; there
is no "unit" to extract and truncate, and `.dr.001/.002` are byte ranges of that single stream —
a part cannot be decoded alone, which is exactly what makes `cat` work.

So **HatchZip's extraction cannot be bolted onto ziptastic as it stands**, and it should not be:
deleting a user's archive as a side effect of decoding is a destructive operation, and the tool
that does it needs HatchZip's whole safety apparatus — the journal, the saved tail, the fit check,
the "whose journal is it" ownership record — not a flag on a compressor.

## What is worth taking: the format property, not the feature

The reason HatchZip can free space is that its formats have **addressable units**. ziptastic
already has them and cannot reach them:

- In **parallel mode** every block is self-contained: it is decoded against itself, references only
  itself, and carries its own SHA-256. Blocks are already independently decodable.
- The stream has **no offset table**, so the decoder can only walk from the front. The trailer
  records the block count and a manifest digest, not where each block starts.

**One format change — a block offset table — would enable four things at once:**

| feature | what the offset table gives it |
|---|---|
| hatch-style extraction | decode from the last block backwards, truncate as each is done |
| random access | decode one block without reading the others |
| resumable decode | restart at a block boundary after a crash |
| parallel decode | hand blocks to threads without a sequential scan first |

That is the honest version of the user's suggestion: **not "add shrinking extraction", but "make
the units addressable, and the shrinking extraction becomes possible later, as its own tool."**

The audit already noted that the offset index from the earlier lexora note was **not built**,
because nothing read it. This is the first thing that would.

## What to take if that tool is ever built

Copy the safety model, not just the idea. Every item below exists in HatchZip because of a failure
mode that is otherwise silent:

- **Check it fits before touching anything**, with a margin kept free the whole way through.
- **Work from the end.** Shortening a file from the end works everywhere; removing from the middle
  needs hole punching, which FAT32 and exFAT do not have.
- **Save the tail first.** ZIP and 7z keep their index at the end, which is the first thing
  truncation removes — so it is copied aside and checksummed before the first truncate.
- **Journal every step, atomically**, so a crash at any moment resumes rather than leaving a
  half-eaten archive with no record of what was already extracted.
- **Ownership record outside the archive's folder.** A journal sits next to the archive, so whoever
  supplied the archive could have supplied the journal — and a resume deletes files. HatchZip
  records the archive and output folder in the user's profile and refuses a journal that does not
  match. This is the part most likely to be skipped and most likely to hurt.

## The multi-format extraction point

ziptastic already *understands* ZIP, gzip, tar-in-gzip, PNG, JPEG, GIF, BMP, SQLite, WAV and raw
CD images — but as things to compress, not as things to unpack for a user. An `x` command that
extracts them would be a different product with a different risk surface (path traversal, symlink
handling, overwrite policy, case-collision folders — all of which HatchZip documents at length).

**That belongs in the orchestrator, not the codec.** heavenzip is the layer that already knows
about pipelines and archives; if a multi-format extractor is wanted, it is a front end over
HatchZip-class logic, and ziptastic stays the compressor it is good at being.

## Recorded conclusion

**Decision, 2026-11-04: shrinking extraction belongs to heavenzip.** Confirmed with the owner. The
reasoning is in the sections above, and the split is clean:

| layer | what it owns | what it does about hatching |
|---|---|---|
| **ziptastic** | the codec | adds a **block offset table** (v8) so its own blocks are addressable. Nothing else. |
| **heavenzip** | the orchestrator | owns the hatch *pipeline stage*: fit check, journal, truncate-as-you-go, resume. |
| **hatchzip** | the extractor | does the actual work for ZIP, 7z, RAR, tar, tar.gz, tar.xz and split archives. |

### For foreign archives: delegate, do not reimplement

heavenzip should **call HatchZip**, not rebuild it. HatchZip is **MIT OR Apache-2.0** — compatible
with ziptastic's MIT — mature, and its `docs/how-it-works.md` documents a dozen failure modes that
were paid for in bugs: the saved tail, the atomic journal, the ownership record that stops a
supplied journal from deleting the user's files, the case-collision folders, the "check it fits
before touching anything". A reimplementation would be months of safety work to arrive at the same
place, and every one of those items is a way to destroy somebody's data.

The integration is small: heavenzip decides *what* to extract and *where*, HatchZip does it.

### For `.dr`: the offset table is the prerequisite

heavenzip can only hatch a `.dr` natively once the format has addressable blocks. Until then the
honest options are "extract fully" (needs the space) or "not yet". That is why the offset table is
Phase 3b in `RELEASE_PLAN.md` and not a later nicety — it is the one thing ziptastic owes heavenzip
before hatching `.dr` is possible at all.

And the offset table earns its place on its own: random access, resumable decode and parallel
decode without a sequential scan are all things ziptastic wants regardless of hatching.

### What ziptastic does not do

It never deletes a user's archive. A codec that destroys its input on success is a codec nobody
can run twice, and the destructive decision belongs to the layer that was told to extract.

### Referenced for later

HatchZip's journal and ownership model, for whoever builds the hatch stage in heavenzip. Read
`docs/how-it-works.md` first; it is the best document of its kind either project has.
