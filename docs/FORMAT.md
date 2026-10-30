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
| metadata          variable |  mode-specific, may be empty
| payload           compressed_size bytes
```

- `uncompressed_size` — bytes this block expands to. Never 0, never above
  1 GiB.
- `compressed_size` — length of `payload`. Must not exceed the bytes remaining
  in the stream.
- `metadata` — defined per mode below. A decoder that does not know the mode
  cannot skip it, so unknown modes are a hard error, not a skipped block.

The decoder additionally enforces that a block cannot claim more output than
the stream's remaining `original_size` budget, and that the bytes a mode
actually produced match `uncompressed_size`.

## Modes

| byte | name | metadata | status |
|---|---|---|---|
| `0x00` | `RAW` | none | **implemented** (Phase 0) |
| `0x01` | `RANS_CTX` | Phase 4 | reserved |
| `0x02` | `HUFF_STATIC` | Phase 7 | reserved |
| `0x03` | `HUFF_ADAPT` | Phase 7 | reserved |
| `0x04` | `BWT_BSC` | Phase 9 | reserved |
| `0x05` | `LZMA2_FALLBACK` | Phase 1 | reserved |
| `0x06` | `BROTLI` | Phase 9 | reserved |
| `0x07` | `ZSTD` | Phase 9 | reserved |
| `0x08` | `LZ4` | Phase 9 | reserved |
| `0x09` | `CM_ZPAQ` | Phase 10 | reserved |
| `0x0A` | `PPMD` | Phase 11 | reserved |

Bytes `0x0B`–`0xFF` are undefined and rejected as `bad-mode`.

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
- **State carries across blocks.** From Phase 2 onward the LZ dictionary, both
  match-finder indexes and the rANS context models persist from block to block;
  the format does not delimit that state, because the decoder rebuilds it by
  decoding in order. This is what makes CHCE one engine rather than a chain of
  independent compressions.
- **Determinism.** The same input with the same options produces byte-identical
  output; the test suite asserts this.
