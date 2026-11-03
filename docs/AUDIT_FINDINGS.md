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

## C. The selector — not yet read

## D. Options and defaults — **FINDING 3 (known, not yet systematic)**

`CompressOptions::precomp` defaults to `false`, which means the library's default behaviour
runs **no Layer 0 transform at all**. This has hidden bugs three times: unit tests that never
exercised precomp, a fuzz test that fuzzed nothing but the backend, and a corpus run whose
`layers` byte was 0x00 and looked correct.

Not fixed here — changing a default is a behaviour change and belongs with D2's test, which
must first prove the CLI and the library agree on what "default" means.

## E. Memory and hostile input — partially done

Two instances of "allocate from a declared number" found and fixed before the audit started
(`block_count`, `dedup_unpack`). The audit's job is to show there is no third; not yet done.

## F. The parallel path — not yet read

## G. Integrity and reporting — one instance found (A4); not yet systematic
