# Format v7: a third layer byte, and the bit it makes room for

Written 2026-11-03. `layers` and `layers2` are both full, so the next Layer 0 capability needs
somewhere to live.

## The change

```
"CHCE" | version(7) | flags | layers | layers2 | layers3 | original_size | true_size | block_count | crc32
                                                 ^^^^^^^^
```

One byte, seven free bits. `kLayer3BmpNested` (bit 0) is allocated and wired.

## What is wired, and what is not

The **nested BMP stage** is in the encoder and the decoder, gated on the container stages, and it
compiles and round-trips. It does **not fire yet**, for the same reason the PNG, JPEG and GIF
nested stages did not fire at first: `bmp_pack` requires the BMP signature at offset 0, and a
container payload puts its own header in front of the member bytes.

The measured reason to finish it: a ZIP of four 128×128 BMPs is **184,929 bytes**, compresses to
**120,879** today, and the plane-split ideal is **1,206 bytes** — a **−99.0%** gap. That is the
largest single number any of these nested transforms has shown, because BMP is uncompressed
pixels inside two DEFLATE layers.

So this commit is the plumbing and the measurement, not the win. The next one is the
anywhere-scan plus the multi-image payload that PNG, JPEG and GIF already have.

## Why a byte rather than reusing a bit

The three layer bytes are separate fields rather than one wide integer so the on-disk shape
stays explicit and a reader can see exactly how many bytes it must consume. That has already
paid: the v6 change added a field, and the only code that broke was tests that hand-built a
header.

## Tests

Two tests hand-build headers and both needed the extra byte — `file_header_impossible_counts`
and `size_mismatch_rejected`. That is the whole cost of the format change, and it is why they
exist: they are the tests that would catch a mis-parse of the header.

32/32 suites pass. v6 and earlier streams are refused with `bad-version` as always.


## The win, one commit later

The nested BMP stage now fires. `bmp_pack` scans for the first BMP anywhere in the payload,
carries a prefix and a tail, and handles **every** image it finds — the same shape PNG, JPEG
and GIF already had.

| case | before | after | gain | `layers3` |
|---|---|---|---|---|
| ZIP of four 128×128 BMPs | 184,929 | **1,530** | **−99.2%** | 0x01 |

Round trip byte-exact, 32/32 suites pass. The plane-split ideal measured beforehand was 1,206
bytes of pixel data; the shipped 1,530 includes the container skeleton and the framing, so the
transform is within a few hundred bytes of the ceiling.

That is the largest gain any transform in this engine has produced on a container payload, and
it is the fourth time the same pattern was applied: **scan for the signature anywhere, carry the
bytes around the image, and handle every image in the buffer.**

The three payload-layout bugs from the earlier transforms (JPEG's tail order, PNG's missing
prefix, GIF's zero-based offset) were all avoided this time by copying the PNG structure exactly
— which is the point of having established it.
