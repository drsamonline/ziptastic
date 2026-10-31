# CHCE bitstream format — version 1

This document is the normative spec for the bytes `ziptastic` writes. It is
written as the engine is built: every mode section is filled in by the phase
that implements it. `include/chimera/format.hpp` is the source of truth in
code; if the two ever disagree, the header wins and this file gets fixed.

## Conventions

- **Integers** are unsigned little-endian **LEB128 varints** unless a fixed
  width is stated. A varint is at most 10 bytes; encodings wider than 64 bits
  are invalid.
- **`u8`** is one unsigned byte.
- A **block** is the unit the mode selector works on. Its uncompressed size is
  at most 1 GiB (`kMaxBlockSize`); it is never zero.
- All sizes are byte counts, never element counts.
- Decoders reject any stream that is not exactly self-consistent: no trailing
  bytes, no size disagreements, no unknown mode bytes.

## Stream layout

```
+--------------------------------------------------+
| magic         "CHCE"                       4 B   |
| version       u8 = 0x01                    1 B   |
| flags         u8                           1 B   |
| original_size varint                            |
| block_count   varint                            |
+--------------------------------------------------+
| block 0                                          |
| block 1                                          |
| ...                                              |
+--------------------------------------------------+
```

### magic

The four bytes `0x43 0x48 0x43 0x45` (`"CHCE"`). Any other value is
`bad-magic`. Note this is *not* a self-extracting header: a CHCE stream is
only readable by `ziptastic` (and by anything that implements this document).

### version

`0x01`. A decoder must reject any other value with `bad-version` rather than
guessing.

### flags

| bit | mask | name | meaning |
|---|---|---|---|
| 0 | `0x01` | `coarse_on` | the coarse (SREP-style) index was consulted while parsing |
| 1 | `0x02` | `max_ratio` | `--max-ratio` was in effect when the stream was written |
| 2 | `0x04` | `precomp_on` | Layer 0 semantic precompression was applied to at least one block |

Bits 3–7 are reserved and **must be zero**; a decoder rejects them with
`bad-flags`. Flags describe how the stream was produced, not how to read it —
a decoder never needs them to decode correctly.

### original_size

Total byte count of the uncompressed input. Capped at 1 TiB
(`kMaxStreamSize`) so a corrupt header cannot trigger an absurd allocation.
The decoder verifies the sum of the block output sizes equals this value.

### block_count

Number of blocks that follow. Because a block is never empty,
`block_count <= original_size`, and an empty input is `original_size = 0,
block_count = 0` with no blocks at all.

## Block layout

```
| mode              u8       |
| uncompressed_size varint   |
| compressed_size   varint   |
| body              compressed_size bytes |
|                            |  = metadata + payload
```

- `uncompressed_size` — bytes this block expands to. Never 0, never above
  1 GiB.
- `compressed_size` — length of the **block body**, which is metadata plus
  payload. For RAW the metadata is empty, so body == payload. Defining it this
  way lets the decoder bounds-check one span before any mode-specific parsing
  happens, and means an unknown mode is a hard error rather than a skipped
  block.
- `metadata` — defined per mode below, and always read out of the body.

The decoder additionally enforces that a block cannot claim more output than
the stream's remaining `original_size` budget, and that the bytes a mode
actually produced match `uncompressed_size`.

## Modes

| byte | name | metadata | status |
|---|---|---|---|
| `0x00` | `RAW` | none | **implemented** (Phase 0) |
| `0x01` | `RANS_CTX` | see below | **implemented** (Phase 4) |
| `0x02` | `HUFF_STATIC` | Phase 7 | reserved |
| `0x03` | `HUFF_ADAPT` | Phase 7 | reserved |
| `0x04` | `BWT_BSC` | Phase 9 | reserved |
| `0x05` | `LZMA2_FALLBACK` | see below | **implemented** (Phase 1) |
| `0x06` | `BROTLI` | Phase 9 | reserved |
| `0x07` | `ZSTD` | Phase 9 | reserved |
| `0x08` | `LZ4` | Phase 9 | reserved |
| `0x09` | `CM_ZPAQ` | Phase 10 | reserved |
| `0x0A` | `PPMD` | Phase 11 | reserved |
| `0x0B` | `LZ_RAW` | see below | **implemented** (Phase 3, transitional) |

Bytes `0x0C`–`0xFF` are undefined and rejected as `bad-mode`.

### `0x01` — RANS_CTX

The parse, coded with context-modelled rANS. The body starts with a model byte
and then, for model 0 (static):

```
| model = 0x00 | 28 frequency tables | payload |
```

and for model 1 (adaptive):

```
| model = 0x01 | varint chunk_tokens | varint total_tokens |
| 28 tables (chunk 0) |
| repeated per chunk: varint payload_len | payload |
```

Model 1 cuts the token stream into chunks of `chunk_tokens` tokens. Chunk 0 is
coded with the tables in the body, measured over the whole block. Every later
chunk is coded with a table derived from the symbols already decoded: both sides
seed their counts from the transmitted frequencies, then decay by three quarters
and add the previous chunk's histogram, never letting a symbol that has been
seen fall below 1. Nothing after chunk 0 is transmitted.

Model 0 is the fast path; model 1 is chosen per block when it is smaller. The
chunk framing costs about 5 bytes per chunk (a length varint and the rANS
flush), which is why model 1 loses on small or incompressible blocks.

The tables, in both models, are:

```
| 4  flag tables   | ctx = position & 3                        | 2 symbols  |
| 16 literal tables| ctx = previous byte >> 4                  | 256 symbols|
| 4  len tables    | ctx = position & 3                        | 32 symbols |
| 4  dist tables   | ctx = min(3, len_slot >> 2)               | 64 symbols |
| payload          | everything remaining                      |            |
```

Each table is written as `varint n_nonzero`, then `n_nonzero` pairs of
`varint symbol, varint frequency`. The frequencies must sum to exactly 4096
(12-bit scale); a decoder rejects any table that does not, because a wrong total
would desynchronise the arithmetic and silently produce garbage.

Lengths: 2..9 map to slots 0..7; longer lengths take a slot plus extra bits.
Distances: the three nearest take slots 1..3; the rest use LZMA's slot pairing.
Remainders are coded as individual bits through a uniform two-symbol table, so
they cost exactly one bit each.

Tables are measured on the block (a static coder). Phase 5 replaces the
measurement with adaptation, which removes the table bytes entirely.

### `0x0B` — LZ_RAW (transitional)

The output of the optimal parse with no entropy coding: literal runs as raw
bytes, matches as a length and a distance.

```
| repeated until uncompressed_size bytes are produced: |
|   0x00  varint count   count bytes |  literal run
|   0x01  varint len     varint dist |  match
```

A match may reference bytes produced by **earlier blocks** — the LZ window
carries across the stream — so the decoder expands into one growing buffer
rather than per block. Overlapping matches (`dist < len`) are legal and are how
runs are expressed.

This mode exists so the Phase 3 parse could be round-tripped and measured
before Phase 4 supplied a coder. It is expected to be **much larger** than
LZMA2 (literals cost 8 bits instead of ~4-5), and Phase 4's `RANS_CTX`
supersedes it. It is kept rather than removed because it is the only mode whose
size isolates the parse from the coder.

### `0x05` — LZMA2_FALLBACK

Body layout:

```
| props_len  varint  |   always 1 today
| props      props_len bytes |  LZMA2 filter property (dict-size code)
| payload    remaining bytes |  a bare LZMA2 stream, no .xz container
```

`payload` is a **raw LZMA2 stream** — the bytes `xz --format=raw` produces, not
an `.xz` file. `props` is the liblzma filter property for LZMA2 (one byte, the
dictionary-size code), so a decoder rebuilds the exact filter without guessing
options. Upstream `xz --format=raw --lzma2=dict=64MiB -dc` decodes any
`LZMA2_FALLBACK` payload produced by this build; the test suite checks that.

The encoder uses preset 9 (64 MiB dictionary) and preset 9|EXTREME, and keeps
whichever is smaller, because extreme is not a superset — it wins by 4.1% on
`xml` and loses by 0.02% on `dickens`.

### `0x00` — RAW

No metadata. `compressed_size` equals `uncompressed_size` and the payload is
the block verbatim. Used for incompressible data and as the Phase 0 baseline;
the mode selector will keep choosing it whenever it wins, since a block is
never allowed to grow beyond a trivial margin.

## Error codes

The decoder returns exactly one of these; nothing throws.

| code | meaning |
|---|---|
| `ok` | stream decoded, output length equals `original_size` |
| `bad-magic` | first four bytes are not `"CHCE"` |
| `bad-version` | version byte is not `0x01` |
| `bad-flags` | reserved flag bits set |
| `bad-mode` | mode byte undefined, or not implemented in this build |
| `bad-varint` | varint truncated or wider than 64 bits |
| `truncated` | stream ended mid-field or mid-payload |
| `bad-block-size` | zero block, block above 1 GiB, or stream above 1 TiB |
| `size-mismatch` | declared sizes disagree with the bytes produced |
| `trailing-data` | bytes remain after the last block |
| `corrupt` | an entropy backend rejected its own payload |
| `io-error` | CLI read/write failure |

## Encoder behaviour notes

These are not part of the wire format, but they explain what a decoder can
assume and what a future version may change.

- **Block size** defaults to 256 KiB (`kDefaultBlockSize`) and is settable
  with `-b`. It is not recorded in the stream: the decoder learns each block's
  size from its own header, so any block size decodes with any build.
- **Headers are written up front**, which means the encoder knows the input
  size before it starts writing. The CLI therefore buffers its input. A
  streaming header variant (block count moved to a trailer) is deliberately
  deferred: it costs ratio (you cannot choose block boundaries without knowing
  the size) and buys nothing for the file-to-file use case.
- **The LZMA2 fallback is whole-stream, not per-block.** A `LZMA2_FALLBACK`
  stream contains exactly one block covering the whole input. Independent
  per-block LZMA2 streams would reset the dictionary every 256 KiB and lose
  badly to `xz -9` on files with long-range redundancy, so the fallback is
  chosen as an alternative to the entire CHCE-native encoding and the
  comparison is on total stream size. Per-block LZMA2 becomes meaningful only
  once blocks carry their own model state, which is what Phase 4's rANS blocks
  do.
- **State carries across blocks.** From Phase 2 onward the LZ dictionary, both
  match-finder indexes and the rANS context models persist from block to block;
  the format does not delimit that state, because the decoder rebuilds it by
  decoding in order. This is what makes CHCE one engine rather than a chain of
  independent compressions.
- **Determinism.** The same input with the same options produces byte-identical
  output; the test suite asserts this.
