# Lexora assessment: what transfers, and in which direction

lexora (`github.com/drsamonline/lexora`) is the sibling project: conservative **text
canonicalisation** plus compression, with `decode(encode(C)) == C` guaranteed.
2,450 lines of Python, Phase-1 prototype, PolyForm Noncommercial, single author.

Same central idea as this repo — transform the data, prove byte-exact recovery, store
a recipe — applied to text instead of containers. Reviewed here to record what
transfers and what does not.

> Both repos are written by the same sole author, so code may be relicensed and moved
> freely. Nothing below is blocked on licence grounds; it is a question of what is
> *useful*, which turns out to be a much narrower set than expected.

---

## What ziptastic should take from lexora — three real improvements

These are engineering patterns, and each maps onto a failure this repo has actually
had.

### 1. Validate before allocate

lexora checks the header hash, the declared limits, the sum of declared sizes and
every offset against the actual file size **before decoding anything**.

ziptastic has had desync bugs whose symptom was a `std::bad_alloc` — a corrupt size
field reaching a `resize()` before anything had validated it. The fix at the time was
a bounds check at the point of failure. lexora's ordering is the structural version:
refuse the whole container up front rather than defending at each use.

**Worth doing**, and cheap: the format is v3 and has a fixed header.

### 2. Per-block integrity with an index and trailer

lexora stores a SHA-256 **per block**, an index of `(offset, compressed_size,
uncompressed_size, sha256)`, and a hashed trailer over the index. A corrupt block
costs that block; the rest of the archive still decodes.

ziptastic has a **single CRC-32 over the whole original**. One corrupted byte loses
the entire archive, and there is no way to recover the surviving blocks. For a 100 MB
archive over a bad transfer, that is the difference between losing 4 MiB and losing
everything.

**Worth doing.** It costs 32 bytes per block plus an index, which at 4 MiB blocks is
under 0.001% overhead.

### 3. Determinism as a stated guarantee

lexora promises "same input + level ⇒ byte-identical archive" and tests it.

ziptastic makes no such claim. Backend selection is deterministic in the serial path,
but the **parallel path** has not been checked, and a non-deterministic archiver is a
problem for anyone doing reproducible builds or deduplicating backups.

**Worth checking and stating**, whichever way it comes out.

---

## What does not transfer — the canonicalisation rules

Measured against this engine's own results, the rules are either negligible or
actively harmful:

| lexora rule | effect on compressed size here |
|---|---|
| **CAN-R01** encoding → UTF-8 | **harmful** — roughly doubles byte count for non-ASCII text |
| **CAN-R02** CRLF → LF | negligible — BWT/PPMd already model `\r\n` as one frequent pair |
| **CAN-R05** soft-hyphen removal | negligible — rare enough not to measure |

CAN-R01 is the interesting one and it is **not a criticism of lexora**. Canonical
UTF-8 and minimum compressed size are different objectives, and lexora has
deliberately chosen the first. But a byte-level compressor downstream pays for it:
the coder must now encode twice as many bytes carrying the same information.

That is the same rule that has predicted every negative result in this repo:

> A transform pays only if it removes redundancy the backend cannot already capture.

A canonicaliser removes *semantic* redundancy — differences that matter to a reader
or a diff tool. A BWT/PPMd backend already collects *statistical* redundancy. The
overlap between the two sets is small, which is exactly what the word-dictionary
result (−0.38% on English, +0.93% on Polish) demonstrated.

---

## The useful direction of integration is lexora → ziptastic

lexora's Phase-1 backend is **zstd-19**. Measured here on Silesia (10 files, 119 MB):

| codec | total |
|---|---|
| `xz -9` | 27,035,452 |
| **ziptastic** | **23,838,035 (−11.83%)** |

`xz -9` beats zstd-19 on most text, and ziptastic beats `xz -9` on every file
measured. So **lexora's ratio is leaving a double-digit percentage on the table**, and
the fix is a codec swap, not a lexora feature.

Its container is already codec-agnostic (`codec_id[16]` in the header), so a
`ziptastic_core.py` beside `zstd_core.py` is the smallest possible change with the
largest possible effect.

A note pointing this out has been added to lexora as `docs/ZIPTASTIC_NOTES.md`, along
with the measured-and-lost table, so the next lexora session does not rebuild a word
dictionary that comes out at −0.38%.

---

## Roadmap additions from this assessment

Added to the queue, in priority order:

1. **Per-block SHA-256 + index + trailer** — turns "one bad byte loses everything"
   into "one bad block loses one block". Small, contained, real.
2. **Validate-before-allocate in `read_file_header`** — refuse malformed containers up
   front rather than defending at each use.
3. **Check and state parallel determinism** — either guarantee it or document that it
   is not guaranteed.

None of these improve ratio. All three improve the property that matters more for an
archiver once it leaves the author's machine: **that a damaged archive loses as little
as possible and that a good one is reproducible.**
