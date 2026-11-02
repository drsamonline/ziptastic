# ECM test material: how it was made, and why that unblocks the transform

Written 2026-11-02. `ECM_MEASUREMENT.md` closed with the honest blocker: the 30.8% figure
came from a synthetic image whose "ECC" was random bytes, and conformance could not be
verified because the sandbox had no real 2,352-byte CD image. `TEST_CORPUS.md` then
recorded that the supplied `.raw` is a *disk* image (2,048-byte sectors), not a CD one.

That is now solved. `mkpsxiso` (GPL-2.0, https://github.com/lameguy64/mkpsxiso) computes
real PlayStation EDC/ECC, and a 40-line local driver around its `EDCECC` class produced a
genuine Mode 2 Form 1 image:

```
2,000 sectors × 2,352 = 4,704,000 bytes
sector 0 sync  00 FF×10 00      mode 02      subheader 00 00 08 00   (form 1)
```

## Licence position

`mkpsxiso` is **GPL-2.0** and ziptastic is MIT, so nothing from it enters the repository.
It was used to produce **data**, which is not code and carries no licence: the generator
links against its EDC/ECC class locally, writes an image, and is then discarded. The image
is test material, not a dependency.

## The parameters, recorded so an independent implementation can be written

A Mode 2 Form 1 sector is `sync(12) header(4) subheader(8) data(2048) EDC(4) ECC(276)`.
The calls that produce it, in mkpsxiso's terms:

| step | what |
|---|---|
| EDC | `ComputeEdcBlock(subheader, 8 + 2048, edc)` — over subheader **and** data |
| ECC P | `ComputeEccBlock(header, subheader, 86, 24, 2, 86, ecc)` |
| ECC Q | `ComputeEccBlock(header, subheader, 52, 43, 86, 88, ecc + 172)` |

Mode 1 differs: EDC covers `header + data` and the zero run, and the ECC input starts at
the header rather than the subheader. Both are worth having in the same transform.

## What this changes

The transform can now be built **and checked against reality**: recompute the 276 ECC
bytes from the 2,064 bytes that precede them and compare with what the image already
holds. If they match byte for byte, the tables and loops are conformant — that is exactly
the check that was missing, and it is the same "verify before use" rule every other
transform here follows, applied to the algorithm itself rather than to the round trip.

Without that check, a wrong implementation would still round-trip perfectly (it would
decline every real image and compress its own output), which is the worst kind of
green test.

## Next

1. Implement RS(26,24) over GF(2⁸) in `ecc_wrap.cpp`, with a self-check against this image.
2. Measure on the real image — the earlier 30.8% was synthetic and should be re-measured
   against real parity, which is a deterministic function of the data rather than random.
3. Take the layer bit from `layers2` (bit 5 is free).
