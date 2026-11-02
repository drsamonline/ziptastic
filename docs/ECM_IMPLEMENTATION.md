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

## Wired in and measured on the real image

| | |
|---|---|
| `test.raw` (2,000 sectors, Mode 2 Form 1) | 4,704,000 |
| compressed, `--no-ecc` | 1,973,243 |
| compressed, with ECM | **1,417,448** |
| **gain** | **−28.2%** |
| the 4,096,000 bytes of user data alone (the ideal) | 1,401,135 |

Round-trip byte-exact, `layers2` 0x20. The transform lands **within 1.2% of the ideal** —
16 KB above compressing the user data on its own, which is the parity-regeneration
overhead plus framing.

The synthetic estimate was 30.8%; the real image gives **28.2%**. The difference is the
caveat that was recorded then: synthetic "parity" was uniform random, whereas real parity
is a deterministic function of the data and carries a little structure the backend can
use. The measured number is the one that matters, and it is 2.6 points lower.

`kLayer2Ecc` is layers2 bit 5. `--no-ecc` turns the stage off.

## Tests

`tests/unit/test_ecc.cpp`, seven cases against a two-sector fixture embedded as a byte
array: round trip, payload drops the parity, **a sector whose parity does not verify is
declined**, non-disc data declined, a partial sector declined, damaged payload rejected,
deterministic packing. 30/30 suites pass.

## What is left

Nothing blocking. The stage is off-by-default-able, verified per sector, and measured on a
real image.
