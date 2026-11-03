# Audit findings: compressor and decompressor

Running log against `docs/AUDIT_PLAN.md`. Each entry says what was read, what was found, and
what evidence closes it. **An entry that says "no finding" is as useful as one that says
"bug"** — it is the record that the section was actually read.

---

## A. The stage chain (encode) — in progress

### A1. Preconditions and partial state — no finding so far

Every stage follows the same shape: `likely()` gate, then `pack()`, then a rebuild and
byte-compare before the payload is adopted. A stage that declines leaves the previous
`stage_*` span untouched, so a failed stage costs nothing but the attempt. Verified by
reading all ten stage blocks in `compressor.cpp`.

### A2. The order is a derivation — **FINDING 1: it is not, and nothing said so**

Read the encode chain and the decode chain and compared them:

```
encode: base64, numeric, wav, bmp, ecc, png, sqlite, jpeg, gif, container stages
decode: container, gzip, numeric, gif, jpeg, bmp, ecc, png, sqlite, wav, base64
```

Those are **not reverses of each other**. `numeric` is undone before `gif`/`jpeg`/`bmp`,
but on encode it ran before them.

It is currently correct, for a reason nobody had written down: only two positions in the
chain are forced. `base64` must run first (every later stage wants the decoded payload) and
the container stages must run last (they need the stream). Everything between them is
mutually exclusive — a file cannot be a WAV and a PNG, a CSV cannot be a raw CD image — so
their relative order never matters, in either direction.

That is a real dependency, and it was invisible. If anyone ever adds a stage that can fire
on a payload an earlier stage produced, the decode order silently becomes load-bearing and
the failure will look like data corruption in one file class.

**Closed by**: the note above `stage_in` in `compressor.cpp`, which states the two forced
positions, names the exclusivity the rest depends on, and says what to check before moving
anything. Also fixed the stale comment on the JPEG stage, which still began "First, because
a JPEG is not DEFLATE…" — it has been eighth for some time.

### A3. Verify-by-rebuild compares the right buffer — checked, no finding

Every stage compares against **its own input** (`stage_wav`, `stage_ecc`, …), not against the
original file. That is the correct comparison: the stage's contract is "I reproduce what I
was given", not "I reproduce the caller's bytes". Confirmed for base64, numeric, wav, bmp,
ecc, png, sqlite, jpeg, gif.

### A4. `original_size` accounting — **FINDING 2 (already fixed, recorded here)**

`DecompressReport::original_size` reported the header's compressed-domain size, not the size
of `out`, so it disagreed with the output for every transformed stream. Found while extending
the fuzz tests, not by reading — recorded here because the audit plan asks for the size
accounting to be checked, and this is what checking it produced.

---

## B. The stage chain (decode) — read end to end

### B1. Mirror order — checked, with a correction to the comments

The inverse chain is **not** the reverse of the encode chain, and section A2 explains why
that is safe: the middle stages are mutually exclusive. Two comments in the decoder claimed
otherwise — "Order is the whole correctness argument" above the nested numeric undo, and
"gzip and precomp are alternatives, both undone after the nested transforms that ran on what
they inflated" — which is true *locally* and misleading globally. Left as written, because
they are accurate about the nested group; the global statement lives at `stage_in` on the
encode side, where a change would actually be made.

### B2. Failure modes — checked, no finding

All eighteen inverse stages return `Err::Corrupt` when their unpacker refuses. None falls
through with a partial buffer, and none ignores a failure. Verified by reading every stage
in the chain.

### B3. No undo can grow past the declared size — **FINDING 4: there was no bound at all**

This is the finding of the audit so far. The chain **expands**, and the only thing standing
between a crafted stream and an out-of-memory kill was the CRC check at the very end of it.

A twenty-byte dedup payload can describe a terabyte — a self-referencing copy (`dist = 1`)
with a huge length — and the decoder would build it byte by byte before discovering the
checksum was wrong. The same applies to any inverse whose output is not bounded by its input.

The format has no field for the true original size (`original_size` is the *compressed-domain*
size), so there is nothing exact to check against. The mitigation now in place:

- a ceiling of **4096 × the declared size + 1 MiB**, checked after **every one of the
  eighteen** inverse stages;
- exceeding it returns `Err::SizeMismatch` immediately, rather than after the allocation.

4096× is far beyond any legitimate stream here — the largest real expansion measured in this
repository is under 100× (a dedup of highly repetitive data) — and far below a bomb.

`a_decompression_bomb_is_refused` pins it: a stream with the dedup bit set and a payload that
cannot be dedup, which must be refused **in under two seconds**.

**The real fix is a declared final size in the next format version**, at which point the
ceiling becomes an exact check. Recorded here so the mitigation is not mistaken for the
solution.

## C. The selector — read

### C1. Candidate size vs body — **FINDING 5: nothing checked it**

Every candidate sets `hdr.compressed_size` by hand, and `write_block_header` then writes that
number followed by `body`. **Nothing compared the two.** The per-block SHA-256 covers the
*plaintext*, not this, so a candidate whose header disagreed with its body would produce a
stream that reads the wrong number of bytes at decode — the next block header lands
mid-payload, and the failure surfaces somewhere unrelated to the cause.

No current backend does this; the risk is the next one. A backend that computes its size
before a final tweak to the body is an easy mistake, and the symptom would be a corrupt
archive rather than a failed build.

**Fixed**: a `write_block` helper compares `compressed_size` with `body.size()` and returns
`Err::SizeMismatch` before a byte is written. Used by the parallel loop, the archive loop and
the whole-stream CM path — the only three places a block is written.

### C2. Failed candidates are dropped — checked, no finding

Every backend appends a candidate only when its compressor returns true. No half-built
candidate can enter the list.

### C3. Never larger than RAW, and Tier-1 on the total stream — checked, no finding

`std::min_element` over the candidate list, which always contains the RAW candidate first, so
the chosen body is never larger than the block. The Tier-1 guarantee is a separate comparison
on the **total stream including framing**, which is where it belongs.

### C4. `--max-ratio`'s second pass — checked, no finding

It replaces the stream only when `cw.size() < native.size()`, strictly smaller, and now also
checks its own header/body consistency before writing.

## D. Options and defaults — read, and it corrected a belief this audit was carrying

### D2. The defaults — **FINDING 6: the belief was wrong, and it was written down three times**

The audit's own notes said `precomp = false` means "the library default runs no Layer 0
transform at all", and that claim was repeated in `VALIDATION.md`, in a commit message and in
the handoff. **It is false.** Reading `compressor.hpp`:

- **Every content transform defaults to ON**: `base64`, `numeric`, `wav`, `bmp`, `ecc`,
  `sqlite`, `jpeg`, `gif`, `dedup`.
- **Two default to OFF**: `precomp` (the container stages — precomp, gzip, the nested level)
  and `coarse` (the SREP-style index).

So the default does not skip Layer 0; it skips the *containers*. A caller who embeds the
library gets base64, CSV, WAV, BMP, CD-parity, PNG, SQLite, JPEG and GIF handling without
asking, and gets nothing for a `.zip`, `.gz` or `.tar.gz` unless they set `precomp`.

The three incidents that produced the wrong belief were real, but each was about **precomp
specifically**: unit tests that never exercised it, a fuzz test that therefore fuzzed only
the backend, and a corpus run whose `layers` byte was 0x00 because the file was a cooked ISO
with no content transform to find.

**Fixed by making the default executable**: `tests/unit/test_defaults.cpp` asserts that a
base64 document is decoded with default options, that a gzip member is *not* inflated with
default options and *is* with `precomp = true`, and that exactly two options default to false
and both are named. The default is now documented by a test rather than by prose that can
drift.

The first version of that test was itself wrong — it buried a 30-byte gzip member in 2 KB of
random bytes, where the gate correctly declines because inflating it gains nothing. It was
testing the gate, not the default. Replaced with a file that *is* a gzip member of repetitive
text.

### D1. Every option honoured both ways — checked, no finding

Each stage is guarded by its own flag, and each flag has a `--no-…` counterpart in the CLI.
`--huff-adapt` is the one flag that turns something *on* rather than off, because its
default was flipped by measurement (`HUFFMAN_GATE.md`).

### D3. Flags written from decisions, not from options — checked, with one note

`kFlagPrecomp` is set from `opt.precomp && precomp`, where `precomp` is the *decision* after
the verify-by-rebuild and the trial — correct. The layer bits are likewise set from the
decision flags. `kFlagCoarse` is the exception: it is set from `opt.coarse` alone, whether or
not the index was consulted, because it records "was in effect" rather than "was used", and
the decoder does not read it. Noted, not changed.

## E. Memory and hostile input — read; there was a third

### E1. Allocation from a declared number — **FINDING 7: `ecc_unpack`, at 2352×**

Enumerated every `reserve`/`resize`/`assign` in the engine and the transforms and checked
each against what bounds it:

| site | bounded by | verdict |
|---|---|---|
| `decompress` output | `min(original_size, 256 MiB)` | ok |
| `decompress` parallel total | `min(original_size, …)` per block | ok |
| `dedup_unpack` | `min(total, packed*32+4096)` | fixed earlier |
| `sqlite_unpack` | `pages_bytes + perm_len + trailer == packed.size()` | ok — exact |
| `png_wrap` | chunk length checked against the payload | ok |
| `bmp_unpack` | `pos + header_len + planes > packed.size()` | ok |
| `wav_unpack` | `samples` counted by walking the payload | ok |
| **`ecc_unpack`** | **`sectors ≤ 2^26` and the payload holds the kinds** | **2352× amplification** |

`ecc_unpack` allocated `sectors * 2352` from a sector count the payload declares, checked
only against 2^26 and against the *kind bytes*. A 64 MB payload can therefore declare 2^26
sectors and ask for **158 GB**. The expansion ceiling added in section B does not help: it
runs *after* this allocation.

**Fixed with a structural bound rather than a guess**: a legitimate payload carries 2076 or
2352 bytes per sector plus one kind byte, so it cannot be shorter than `sectors × 2077`. The
output is then at most 1.13× the payload by construction, and no number the stream declares
can size an allocation larger than the bytes it brought with it.

`a_declared_sector_count_must_be_carried` pins it: 2^26 sectors declared, two kind bytes
supplied, refused in under a second with nothing allocated.

### E2. Loops terminate on input exhaustion — checked, no finding

Every length-reading loop either advances `pos` past consumed bytes or is bounded by a
counter that is itself bounded by the payload (`sqlite`'s page loop, `gif`'s index loop,
`wav`'s varint walk). No loop counts to a declared number without touching the input.

### E3. Size arithmetic — checked, no finding

The additions that matter are done in `uint64_t` and compared before use
(`pages_bytes + perm_len + kTrailer != packed.size()`, `pos + keep > packed.size()`,
`bh.uncompressed_size > fh.original_size - produced`). The last is the overflow-safe form:
it subtracts rather than adds.

## F. The parallel path — read

### F2. Shared state between workers — **FINDING 8: a data race on `stats.tokens`**

Listed everything the block workers touch:

| object | sharing | verdict |
|---|---|---|
| `encoded[b]` | one element per block, written by whichever worker takes it | safe — distinct objects |
| `next` | `std::atomic<size_t>` | safe |
| `finder`, `model`, `local` | constructed inside the worker | safe |
| `input`, `opt` | read-only | safe |
| `bwt_bsc` init | `std::call_once` | safe |
| **`stats.tokens`** | **`+=` from every worker** | **data race** |

`stats.tokens += local.tokens` inside the worker is a non-atomic read-modify-write on a
shared struct: undefined behaviour, and a lost update in practice, so the `-v` token count
drifted with thread scheduling.

**Fixed**: per-block counts go into a `std::vector<uint64_t>` indexed by block, summed after
the join. No atomics, no contention, and the value is now deterministic.

`parallel_token_count_is_deterministic` pins it — five runs over 16 blocks must agree — and
writing that test turned up two things worth keeping:

- The first fixture was incompressible bytes, where the parser legitimately produces **zero**
  tokens, so the "tokens > 0" guard proved nothing.
- The second fixture compressed so well that the **Tier-1 fallback won**, and the stats then
  describe the *fallback* stream (one block, no tokens) rather than the parallel one. The
  test now disables the fallback so it measures what it claims to.

### F1. Parallel and sequential agree — checked, and they are not supposed to

The two modes are documented as producing different bytes by design: parallel blocks are
self-contained (no cross-block matches, no coarse index, no carried dictionary) while archive
mode keeps them coupled for ratio. That is a documented, intentional difference, not a
divergence to be tested away. Both round-trip exactly, which the existing tests cover.

### F3. The concurrent fallback — moot

The experiment that ran the Tier-1 fallback on a second thread was measured and **reverted**
(`CONCURRENT_FALLBACK.md`): it was 9% slower on this machine. There is no second thread in the
shipped encoder, so there is no half-initialised state to audit.

## G. Integrity and reporting — one instance found (A4); not yet systematic
