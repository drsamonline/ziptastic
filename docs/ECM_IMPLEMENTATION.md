# ECM: the transform is written, and it verifies against a real disc image

Written 2026-11-02. Status: **`src/precompressors/ecc_wrap.{hpp,cpp}` written and verified
against a real Mode 2 Form 1 image. Not yet wired into the engine** — no layer bit, no
stage, not in `CMakeLists.txt`. That is the next commit, not this one.

## What it does

Regenerates the 276 bytes of Reed-Solomon parity in every CD sector instead of storing
them. Payload is the sectors with the parity removed, plus one kind byte per sector:

| kind | meaning |
|---|---|
| 0 | parity regenerated, address field = sector header |
| 1 | sector kept whole (Mode 2 Form 2 carries no parity) |
| 2 | parity regenerated, address field = four zero bytes |

## Verified against reality

`ECM_TEST_MATERIAL.md` produced a 2,000-sector Mode 2 Form 1 image with mkpsxiso's EDC/ECC.
The transform recomputes every sector's parity and compares it with what the image already
holds — **all 2,000 match**, and the round trip is byte-exact:

```
likely=1 size=4704000 sectors=2000
pack=1 packed=4154002 in=4704000
unpack=1 equal=1
```

That check is the whole point of building it this way. A wrong table or loop would still
round-trip perfectly on its own output while declining every real image, which is the
worst kind of green test. Here, a wrong implementation cannot pass.

## Three things it cost, all caught by that check

**1. The Mode 2 codeword is not the Mode 1 codeword.** Mode 1's 2,064 bytes start at
offset 12 (header, data, EDC, zero run). Mode 2 Form 1's start at the *subheader*, with a
4-byte address field in front — reading 2,064 bytes from offset 16 instead runs past the
EDC into the parity itself.

**2. Two address conventions exist.** The address field is either the sector header
(what mkpsxiso writes, and what the stored parity matched) or four zero bytes. The
transform tries the header first, then zeros, and records which one per sector. Assuming
either would have declined real images from half the tools in the world.

**3. Q covers P.** This is the one that took a second pass. Q is computed over
52 × 43 = 2,236 bytes, which is *larger* than the 2,064-byte data region, because it also
covers the 172 P parity bytes written moments earlier. A private 2,064-byte copy of the
codeword computes Q over the wrong bytes and produces 104 parity bytes that look entirely
plausible — the first version did exactly that, and the byte-by-byte comparison against
mkpsxiso found it at index 172, the first byte of Q.

## What is left

1. Wire it in: `kLayer2Ecc` (layers2 bit 5, free), a stage in `compressor.cpp`, the
   inverse in `decompressor.cpp`, and `CMakeLists.txt`.
2. Measure on the real image. The 30.8% from `ECM_MEASUREMENT.md` was synthetic; real
   parity is a deterministic function of the data, so the number may move.
3. Unit tests with the image embedded, or generated in-test the way `test_sqlite.cpp`
   builds its fixture.
