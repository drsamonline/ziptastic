# BMP colour planes: 98.9% on a photographic image, declined on a sprite sheet

Written 2026-11-02. Layer bit `kLayer2Bmp` (layers2 bit 6).

## Result

| file | without | with | gain | `layers2` |
|---|---|---|---|---|
| 512×512 photographic-ish, 786,486 B | 127,319 | **1,375** | **−98.9%** | 0x40 |
| 256×256 sprite sheet, 196,662 B | 365 | 365 | **declined** | 0x00 |

Both round-trip byte-exact.

## What the transform does

An uncompressed BMP is raw BGR pixels, row by row. Two things make that a poor shape for
a compressor: the three channels are interleaved, so a match has to step three bytes at a
time, and neighbouring pixels differ by a little rather than being equal. The transform
splits the channels into separate planes and stores each pixel as the difference from the
pixel to its left, resetting at each row.

## Why the gate is not optional

The same transform **hurts** the sprite sheet: 365 → 640 bytes with the delta, 365 → 521
with the split alone. That file is flat colour blocks, which the backend already encodes
almost for free; splitting the planes destroys the row structure it was using and the
delta replaces runs of identical bytes with runs of zeros followed by occasional 1s.

So the stage is gated by `precomp_helps`, the same trial PNG and precomp use: build the
payload, compress both, keep it only if it wins. On the sprite sheet it declines and the
output is byte-identical to before, which is exactly what the `layers2 = 0x00` in the
table shows.

This is the clearest case in the project of a transform that is right for one image and
wrong for another — the reason every stage here is gated rather than applied.

## Format notes

- 24-bit and 32-bit BI_RGB only. RLE and bitfields are different transforms, not this one,
  and are declined.
- The row stride includes the 4-byte alignment padding; the payload drops it and the
  decoder re-pads from the width. A test covers a 37-pixel-wide image, where the stride is
  not the row length.
- The height sign only says which way up the image is; the transform works in file order
  either way.

## Tests

`tests/unit/test_bmp.cpp`, eight cases: 24-bit and 32-bit round trips, an odd width with
padding, payload size, RLE declined, non-BMP declined, truncated declined, damaged payload
rejected. 31/31 suites pass.
