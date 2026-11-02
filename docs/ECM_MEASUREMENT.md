# ECM for raw CD images: measured at 31%, not the 12% we assumed

Written 2026-11-02. `SESSION_HANDOFF.md` listed "ECM for **raw** CD images only (~12%)"
near the bottom of the queue. Measured the ceiling before building anything, and the
number is three times larger than the estimate — it should move up the queue.

## The measurement

A synthetic raw Mode-1 image: 2,000 sectors of 2,352 bytes = 12 sync + 4 header + 2,048
user + **288 ECC**. The user data is a realistic mix — English text, numeric tables,
already-compressed blobs, and a filename index — so the user half is not trivially
compressible.

| | bytes | compressed |
|---|---|---|
| raw image (with ECC) | 4,704,000 | **1,924,173** |
| user data only (the ideal once ECC is regenerated) | 4,096,000 | **1,331,553** |

**Ideal gain: 30.8%.**

## Why the estimate was wrong

The 12% figure is the *uncompressed* arithmetic: 608,000 bytes of sync/header/ECC out of
4,704,000 is 12.9%. But Reed-Solomon parity is designed to look random, so it is
**incompressible** — it costs its full size in the compressed stream, while the user data
around it compresses 3.1×. In compressed terms the parity is 592,620 of 1,924,173 bytes,
which is 31%.

That is the same lesson as the base64 work: the gain is not the fraction of bytes you
remove, it is the fraction of the *compressed* output those bytes were costing.

## What building it involves

ECM does not store the ECC — it regenerates it. For Mode 1 that is the RS(26,24) product
code over GF(2⁸): P parity over 43 columns, Q parity over 26 diagonals, 288 bytes per
sector, computed from the 2,064 bytes of sync+header+user. Mode 2 form 1 has a different
layout. The transform would:

1. recognise a raw image by its sector sync pattern (`00 FF×10 00`) and 2,352-byte
   stride, and by the mode byte;
2. emit the sectors with the ECC regions removed, plus a mode tag;
3. on decode, recompute the parity and splice it back;
4. verify by byte-comparison, and decline anything whose ECC does not match what the
   tables produce — which is also a correctness check on the tables themselves.

Cost: GF(2⁸) log/antilog tables (512 bytes), two parity loops per sector, and a
format-shape parser. No new format bit is needed beyond a `layers2` bit.

## Caveat on the number

The 288 ECC bytes here are uniform random, which is what real parity looks like but not
what it is — real parity is a deterministic function of the user data, so a coder that
models the relationship could in principle squeeze a little out of it. That would make
the *measured* gain smaller than 30.8%, not larger. The right check is the same one every
transform here gets: measure on a real `.bin` after it is built, and decline when the
round trip or the size does not hold up.

## Status

Not built. Measured ceiling 30.8% on a realistic synthetic image. This is the largest
unclaimed win in the queue, well above the ~12% it was budgeted at, and it should be the
next Layer 0 item rather than the last.
