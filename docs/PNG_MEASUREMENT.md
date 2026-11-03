> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# PNG image data inflated through preflate — shipped

Written 2026-11-02. Layer bit `kLayer2Png` (layers2 bit 4).

## Result

| file | without | with | gain | `layers2` |
|---|---|---|---|---|
| 256×256 sprite sheet, 2,051 B PNG | 1,632 | **1,258** | **−22.9%** | 0x10 |
| 512×512 gradient, 376,770 B PNG | 364,900 | **113,742** | **−68.8%** | 0x10 |

Round-trip exact. The prototype predicted −26.2% and −69.0%; the shipped transform lands
within 3 points on the small file and a tenth of a point on the large one.

## Why this is nearly free to build

A PNG is a DEFLATE stream (its IDAT chunks) inside a chunk list. precomp exists to undo
exactly that, but it only ever reached ZIP and PDF, so PNGs were handed to the backend
as opaque bytes. The transform is therefore the **gzip pattern**: walk the container, hand
the DEFLATE stream to preflate, keep everything else verbatim. Nothing re-encodes zlib,
so there is no "our zlib differs from theirs" class of bug — preflate reproduces the
original stream byte for byte.

## Two traps, both found by the first run declining every PNG

**1. PNG's IDAT stream is zlib-WRAPPED.** A ZIP member is bare DEFLATE; PNG adds a 2-byte
zlib header and a 4-byte Adler-32. `preflate_decode` rejected every stream until the
wrapper was peeled off and stored verbatim — six bytes that cannot be mis-reproduced.
The transform silently declined 100% of inputs before that fix, which read as "PNG is not
worth handling" rather than "the wrapper was wrong".

**2. The per-chunk CRC is not in the skeleton.** The skeleton holds every non-IDAT chunk
whole, so an IDAT chunk's CRC has nowhere to live. It travels with the chunk length
(four bytes each). Computing it instead would mean shipping a second CRC-32 whose
polynomial has to match zlib's exactly — storing four bytes cannot be wrong.

## What it means for a corpus

PNGs are the most common image format in software, and the ones that gain most are the
ones a compressor cannot already handle: smooth gradients, sprites with flat regions,
screenshots. The 68.8% on the gradient is the interesting number — that file is
essentially incompressible as a PNG (376 KB → 365 KB), because zlib has already taken
everything it can. Inflating it exposes 787 KB of scanlines that the engine compresses to
113 KB.

## Files

- `src/precompressors/png_wrap.{hpp,cpp}` — built into the C++17 preflate shim, because
  it includes preflate's headers.
- `include/chimera/format.hpp` — `kLayer2Png` (layers2 bit 4).
- `src/engine/compressor.cpp`, `decompressor.cpp` — the stage and its inverse.
- `tests/unit/test_png.cpp` — 6 cases, fixture embedded as a byte array.
