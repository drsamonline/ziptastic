> **Superseded.** For current figures, [`FINDINGS.md`](FINDINGS.md); for the current format,
> `FORMAT.md` and [`FORMAT_V8.md`](FORMAT_V8.md). This document is the working record of v7 — how
> the number was arrived at and what was corrected along the way. Where the two disagree,
> `FINDINGS.md` is right.

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


## The win, one commit later — and a correction to the number

The nested BMP stage now fires: `bmp_pack` scans for the first BMP anywhere in the payload,
carries a prefix and a tail, and handles **every** image it finds.

The first measurement reported **−99.2%** on a ZIP of four 128×128 BMPs (184,929 → 1,530). That
number is real for that fixture and **misleading as a claim**, because the fixture was four
synthetic gradients whose pixels are a formula — `((x*3+i*20)%256, (y*5)%256, (x*x+y*y+i*7)%256)`
— and those compress to almost nothing under any transform. Checked against real content:

| case | zip | ziptastic `-9` | gain | vs `xz -9e` |
|---|---|---|---|---|
| ZIP of a photographic BMP, 320×240 | 225,471 | **142,160** | **−36.9%** | +36.9% |
| ZIP of a photographic BMP, 512×512 | 747,474 | **386,702** | **−48.3%** | +47.7% |
| ZIP of pure-noise BMP, 256×256 | 196,838 | 196,913 | −0.0% | −0.0% |
| ZIP of photo + noise | 422,287 | **344,254** | **−18.5%** | +18.5% |

All four round-trip byte-exact. The noise control declines the transform entirely
(`layers3 = 0x00`), which is the gate doing its job.

**So the honest range is −19% to −48%, not −99%.** The transform is still a large win — a
photographic BMP inside a ZIP is two DEFLATE layers over raw pixels, and removing both is worth
more than any general compressor gets — but the number that belongs in a summary is **about
40% on photographic content, nothing on noise**, and the −99% figure should be read as "this
fixture was a formula".

The lesson is the same one this project keeps relearning: **a synthetic fixture can measure the
transform instead of the content.** The gradient images were chosen to make the plane-split
visible; they made it look like magic.

## What is unchanged

The scan, the multi-image payload, the prefix/tail handling, the round trip and the format are
all as described above. Only the headline number was wrong, and the correction is cheaper than
the embarrassment of shipping it.
