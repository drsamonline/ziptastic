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


## Correction, 2026-11-03: the −98.9% was a formula, not a photo

The table above reports **−98.9%** on `photoish.bmp`, a 512×512 image whose pixels are
`((x*3+y*5)%256, (x*7)%256, (x*x+y*y)%256)`. That is a formula, not a photograph, and it
compresses to almost nothing under any transform — the plane split was measuring the fixture,
not the transform.

Re-measured on real content, inside a ZIP (the nested case):

| content | gain |
|---|---|
| photographic BMP, 320×240 | **−36.9%** |
| photographic BMP, 512×512 | **−48.3%** |
| pure noise | **−0.0%** (declined) |
| photo + noise | **−18.5%** |

The top-level numbers are in the same range: a real 320×240 photo goes 230,454 → 142,058
(**−38.4%**) through the transform, while the synthetic gradient went to 628 bytes.

**The transform is worth about 40% on photographic pixels and nothing on noise.** The earlier
figures were not wrong for their fixtures; they were the wrong fixtures. Everything else in this
document — the gate, the sprite-sheet decline, the round-trip property — stands.
