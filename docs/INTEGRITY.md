# Per-block integrity: SHA-256, the trailer, and what it costs

Written 2026-11-01. Numbers are from this session on the machine described in
`BENCHMARKS.md`.

## What was wrong

The format carried one CRC-32 over the whole original, checked after every
inverse transform had run. Two consequences:

1. **It names nothing.** A damaged archive tells you the archive is bad. It never
   tells you which block, and there is no way to recover the good blocks.
2. **It answers last.** The CRC is computed on the fully rebuilt output, so every
   block is decoded, every Layer 0 inverse is applied, and only then does the
   answer arrive. On a 100 MB archive that is all the work, spent on data that was
   already known to be untrustworthy.

A 4 MiB block is worth 32 bytes. That was the whole argument.

## What the format does now (v4)

```
"CHCE" | version u8 = 4 | flags u8 | layers u8 | original_size varint
       | block_count varint | crc32 u32
per block: mode u8 | uncompressed_size varint | compressed_size varint
         | sha256[32] | metadata | payload
trailer:   "ZTIX" | block_count varint | sha256(manifest)[32]
```

**Per-block digest** — SHA-256 of the block's *decoded plaintext*, i.e. the bytes
that block contributes before any Layer 0 inverse runs. Verified as each block
comes out of the decoder, in both the sequential and the parallel path, before the
bytes are concatenated or handed to the transform stack.

**Manifest** — per block, in order: mode byte, both sizes, and the block's own
digest, hashed together into one 32-byte value carried in the trailer. This binds
the *set* of headers, so a dropped, duplicated or reordered block, or an edited
header field, fails before any payload is decoded. The trailer is 37 bytes for the
whole stream, not per block.

**The failure names the block.** `Err::BlockChecksumMismatch` plus a block index in
`DecompressReport`, which the CLI prints:

```
$ ziptastic d noise_bad.zt
ziptastic: decompression failed: a block failed its SHA-256 check (block 2 of 13)
```

## Measured cost

**Bytes.** 69 bytes for a single-block stream (32 block digest + 37 trailer), and
32 more for every additional block.

| file | v3 | v4 | delta |
|---|---|---|---|
| `book.xlsx` (524,118 B, 1 block) | 73,367 | 73,436 | +69 |
| `sheet1.xml` (3,250,852 B, 1 block) | 72,464 | 72,533 | +69 |
| `book.tar.gz` (208,630 B, 1 block) | 208,652 | 208,721 | +69 |

At the default 4 MiB block a 100 MB archive pays 25 × 32 + 37 = **837 bytes,
0.0008%**. At `-b 1` (1 KiB blocks) it is 32 bytes per 1 KiB, **3.1%** — the one
configuration where this is not free, and it is not a configuration anyone uses
for real data.

**Time — and this is the part worth arguing about.** SHA-256 here runs at
**140.8 MB/s, 28.4 ms per 4 MiB block**, measured directly against the compiled
implementation. Decoding the 3.25 MB sheet stream takes **102.7 ms** wall
(31.6 MB/s, process start included), of which the digest is **23.1 ms — about
22%**. The engine's measured decode speeds are 20–70 MB/s, so the digest is
roughly a quarter of decode time.

That is not a rounding error, and it should be a deliberate choice rather than
something that arrived with the feature. Three ways to spend less, in the order I
would try them:

1. **SHA-NI.** x86-64 with the SHA extension runs SHA-256 at 1.5–2 GB/s, which
   would put the cost near 2%. Needs runtime dispatch and an intrinsic path; the
   scalar implementation stays as the fallback.
2. **Per-block CRC-32 instead.** ~1–4 GB/s, essentially free, and for *random*
   corruption — the failure this format actually sees — it is as good. What it
   loses is collision resistance and any claim of tamper evidence, which this
   format does not make anyway (the whole-file CRC-32 never did).
3. **Keep SHA-256 but make it optional**, with a header field naming the algorithm
   per stream.

I shipped SHA-256 because that is what the lexora assessment asked for and because
32 bytes that cannot be forged is the stronger default. The cost is real and
recorded here rather than discovered later.

## The v3 break

v3 streams are no longer readable; `read_file_header` rejects them with
`bad-version`. This is the same clean break as v1→v2 and v2→v3, and for the same
reason: the block header changed shape, and a decoder that guessed whether a block
carries a digest would be worse than one that refuses.

## Verification

- **SHA-256 itself** is pinned to FIPS 180-4 test vectors in
  `tests/unit/test_integrity.cpp`: empty string, `"abc"`, the 56-byte two-block
  vector, and 1000 × `'a'` (which crosses the padding boundary differently). If
  these pass, the digest is the standard's and not an approximation of it.
- **A damaged block** is caught with `raw_options()` forcing RAW blocks, so the
  body *is* the plaintext and the flipped byte reaches the hash rather than being
  rejected by a backend first: `Err::BlockChecksumMismatch`.
- **A damaged trailer** — a flipped digest byte and an edited block count — gives
  `Err::ManifestMismatch`.
- **A missing trailer** gives `Err::Truncated`; **bytes after it** give
  `Err::TrailingData`.
- **The parallel path** is tested separately, because it assembles blocks from
  threads and would be easy to leave unchecked.
- **The single-block writers** (`encode_lzma2_stream`, the `--max-ratio` zpaq
  path) are covered, since both build their own header and both would otherwise
  have produced streams that compress and then refuse to decode.

## Files

- `include/chimera/sha256.hpp`, `src/engine/sha256.cpp` — FIPS 180-4, self-contained.
- `include/chimera/format.hpp` — `kFormatVersion = 4`, `Err::BlockChecksumMismatch`,
  `Err::ManifestMismatch`.
- `include/chimera/block.hpp` — `BlockHeader::sha256`, `kTrailerMagic`,
  `BlockManifest`, `write_trailer` / `read_trailer`.
- `src/engine/block_writer.cpp` — serialisation of all of it.
- `src/engine/compressor.cpp` — one `seal()` helper used by all four write sites
  (parallel, archive, LZMA2 stream, zpaq), plus the trailer.
- `src/engine/decompressor.cpp` — per-block check in both decode paths, manifest,
  trailer, and the block index in the failure.
- `src/main.cpp` — the failure message names the block.
- `tests/unit/test_integrity.cpp` — 11 cases.

## One process note

Disabling ninja's depfile tracking in this sandbox (needed because the 300 s
command cap kept killing builds mid-edge) means **header changes do not trigger
recompiles**. Editing `format.hpp` and rebuilding gave a test binary compiled
against the old version constant, which cost an hour of chasing a phantom. Touch
the sources, or restore the depfiles, before believing a rebuild.
