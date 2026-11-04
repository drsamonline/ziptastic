# Format v8: a block offset table

Written 2026-11-04. This is Phase 3b of `RELEASE_PLAN.md`, and the one thing `HATCHING.md` says
ziptastic owes heavenzip before a `.dr` can be hatched at all.

Current numbers are in `FINDINGS.md` §3c. This document is the working record: what the format
change is, why it is shaped this way, what it measured, and what it deliberately does not do.

## The problem

A v7 stream records how many blocks it has and a digest over their headers. It does not record
where any of them starts, so the only way to reach block 200 is to parse the 199 headers in front
of it. That is not expensive in itself — headers are 35–45 bytes — but it means the reader has to
walk the whole stream, and a stream is a byte range that may not all be in memory, or on disk, or
still there.

The blocks were already independently decodable. In parallel mode each is encoded against itself
and carries its own SHA-256. They were simply unreachable.

## The change

No header field moved. The header is byte-identical to v7; only the trailer grew:

```
"CHCE" | version(8) | flags | layers | layers2 | layers3
       | original_size | true_size | block_count | crc32

per block: mode | usize | csize | sha256[32] | metadata | payload

trailer:  "ZTIX" | block_count varint | sha256(manifest)[32]
        | block_offset[0..count-1], each u64 little-endian
        | "ZTOX"
```

Each entry is the absolute position of a block's **header**, counted from the first byte of the
stream — the `C` of `CHCE` — so an SFX stub or a joined split set in front of the stream changes
nothing. A block's payload begins `header_length` bytes after its entry, which is what the reader
computes when it gets there.

`version` still has to change, and v7 streams are still refused with `bad-version`. A v7 trailer
has no table, and a reader that guessed would read the manifest as offsets.

## Why fixed-width, and why a tail marker

Three shapes were considered.

**Delta-coded varints at the end.** About 3 bytes a block instead of 8 — 1.1 KB saved on a
225-block archive. Rejected because a varint table can only be walked from one end: to find its
first entry you have to know where it starts, and the only way to know that is to parse backwards
through it, which is exactly the sequential dependency the table exists to remove. The saving is
0.08% of an archive and the cost is the feature.

**A length-prefixed table in front of `ZTIX`.** Self-locating from the front, not from the end, and
a reader arriving at the end of the file has to scan back for the magic — through the table it is
looking for.

**Fixed-width u64, terminated by `ZTOX`.** Chosen. The trailer is a fixed size for a given block
count (`trailer_bytes()` in `block.hpp`), the count is in the header, so a reader computes
`end - trailer_bytes(count)` and lands on `ZTIX` without parsing anything. The tail marker is four
bytes that distinguish a complete stream from one truncated inside its table, and give the check
that says so a name: `corrupt`, not a length field that happens to be wrong.

Eight bytes a block against a block that is 16 KiB–4 MiB of input is not a cost worth optimising.
On a one-block archive it is 12 bytes — see "What it costs" below, where that number is not
flattering.

## What validates it

The table is data in the stream, so it can be wrong. Four independent checks, none of which decodes
a payload byte:

1. **Structure** — the two magics, the count, and strictly increasing offsets (`read_trailer`).
2. **Tiling** — `read_index` follows every entry to a block header, parses it, and requires the
   blocks to start where the file header ends, to be contiguous, and to end exactly where the
   trailer begins. A table that points into a payload, or leaves a gap, fails here.
3. **The manifest** — the headers found at those offsets are re-hashed into the manifest digest and
   compared with the stored one. This is the check that makes the table *true* rather than merely
   well-formed: it covers every block's mode, both sizes and its own digest, in order.
4. **Agreement with the walk** — a decoder that walked the blocks knows where they were, so
   `decompress()` passes what it observed to `read_trailer` and a table that disagrees is refused
   with `manifest-mismatch`. The two readers cannot quietly disagree about the same file.

The consequence: a corrupted table costs a refusal, never a wrong answer. Tests for each of the
four are in `tests/unit/test_offsets.cpp`, including a table edited to point eight bytes into a
payload while still increasing.

## What it enables, measured

`tests/benchmark/index_cost.cpp`, on 3.68 MB of concatenated project documentation, 16 KiB blocks,
225 blocks, 1,377,975-byte archive:

| | |
|---|---|
| `read_index` | **0.04 ms**, touching **10,414 of 1,377,975 bytes (0.76%)** |
| decode the whole stream | 888 ms |
| decode one block, from its offset | **4–11 ms** (two runs; this machine has two cores and a noisy neighbour) |

So: reaching one block out of 225 costs 0.76% of the archive in I/O and about 1/100th of the decode
time, and it is a property of the format rather than of the machine. The four features the release
plan listed:

- **random access** — `decode_blocks(in, index, n, n, out)`, and `ziptastic d --block N`;
- **resumable decode** — `--from-block N`, which is what a crashed decode restarts with;
- **parallel decode without a sequential scan** — the index hands out every block's position before
  a thread touches anything;
- **hatch-style extraction** — heavenzip's to build, on top of the above. ziptastic still never
  deletes its input.

## What it deliberately does not do

**A partial decode of a transformed stream is refused, not answered.** `decode_blocks` returns
`not-self-contained` unless the stream was written with `--parallel` *and* no Layer 0 or
stream-level transform ran. Both halves are necessary: an archive-mode block may reference an
earlier block, and the inverse chain needs the whole stream. Returning a block's plaintext when it
is an intermediate of that chain would be plausible, wrong, and undetectable by the caller.

The CLI says which of the two it is, because the fix is different in each case:

```
$ ziptastic d --block 2 archive.dr
ziptastic: block 2 is not independently decodable: this stream is in archive mode, where a block
may reference an earlier one.
  re-compress with --parallel to make every block self-contained
```

**It does not make archive mode addressable.** Archive mode is where the ratio is, and it is
inherently sequential. The table is written for those streams too — a decoder that seeks, or that
wants to know where a block is without reading it, still benefits — but `--block` on one is a
refusal by design.

## What it costs

The table is `8 × blocks + 4` bytes more than a v7 trailer of the same stream.

| case | archive | blocks | added | share of the archive |
|---|---|---|---|---|
| 3.68 MB of docs, 16 KiB blocks | 1,377,975 | 225 | 1,804 | 0.131% |
| 460 KB of docs, 64 KiB blocks | 145,164 | 8 | 68 | 0.047% |
| 460 KB of docs, default block size | 118,284 | 1 | 12 | 0.010% |
| **1 KB of text** | **687** | **1** | **12** | **1.7%** |

The last row is the one to be honest about: on a small single-block archive the table is 1.7% of
the output, and it tells the reader nothing it could not have worked out. The same class of problem
as the 1.2 KB PNG in `FINDINGS.md` §2, where framing was a large fraction of a small file.

It was left in anyway, for now, because the alternative is a second trailer shape and a flag to
distinguish them — and flags are full, so it would mean another header field, which is a bigger
change than the problem deserves. If small files ever matter more than format uniformity, the
honest fix is a trailer form without a table for `block_count == 1`, and it is one `if` in the
writer and one in the reader. Recorded here so it is a decision rather than an oversight.

## Tests

A new suite, `tests/unit/test_offsets.cpp`, 14 cases: the trailer's arithmetic, every offset
landing on its block, the index agreeing with a front walk, the index reading no payload, single-
block and empty streams, a block decoded from its offset matching the full decode, refusals for
archive mode and for transformed streams, a damaged block named by index, a corrupted table, a
truncated tail, a prefixed (SFX) stream, and the walking decoder catching an edited table.

Three existing tests in `test_integrity.cpp` counted backwards from the end of the stream (`-33`,
`-8`, "the last 37 bytes"). They now compute the trailer's position from `trailer_bytes()`, because
the version of that test that hard-coded 37 would have kept passing while flipping a byte of the
offset table instead of the manifest digest.

**33 suites, all green. 53 CLI assertions, all green.** (1.0.0 was 32 and 39.)
