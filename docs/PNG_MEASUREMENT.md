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

## What the transform costs in seconds (2026-11-17)

The numbers above are all ratio. The cost side was never written down, and a 93 KB PNG
taking 25 seconds is the kind of thing that gets reported as a hang. Measured on
`docs/shot.png` — 1,280×900, a UI screenshot — one thread, `ziptastic 1.4.3`:

Measured on **1.4.3**, before the unfilter trial below changed the default — the absolute
numbers are a 1.4.3 baseline, not today's.

| run | bytes | time |
|---|---|---|
| `-9` | **67,217** | **25.7 s** |
| `-9 --no-precomp` (transform off) | 86,626 | **0.70 s** |
| `-9 --no-parse` | 70,487 | 15.1 s |
| `-9 --no-bwt` | 67,217 | 26.2 s |
| `-9 --no-lzma2` | 67,217 | 23.8 s |

**Where the 25 seconds go.** The file is 92,822 bytes; its IDAT stream inflates to
**3,456,900 bytes — 37×** — and the transform's price is running the whole block loop over
that. 3.46 MB in 25.7 s is **131 KB/s**, which is two to five times slower than the
engine's own stated 0.3–0.6 MB/s (`FINDINGS.md` §7). Inflated pixels are the worst case
for a match finder: rows repeat by a few bytes at a time and almost nothing is long, so
the optimal parse spends its time finding short matches that the range coder then prices.

So the 25 seconds are not a bug and not waste — they are the price of 22.4%
(86,626 → 67,217) on a file whose content the engine has no matches for.

**The two knobs that exist**, for a caller who would rather have seconds:

| flag | time | cost |
|---|---|---|
| `--no-precomp` | 0.70 s | +28.9% size (the transform's whole win, gone) |
| `--no-parse` | 15.1 s | +4.6% size |

`--no-bwt` changed neither the bytes nor the time on 1.4.3, which means the BWT candidate
ran on every block of inflated pixels and lost every time. (Re-checked 2026-11-18 against
today's build: `--no-bwt` still equals the default, 51,822, so the claim holds while the
absolute number has moved on.) It is not the cost — it is 0.5 s of
noise — but it is a candidate that cannot win on this content and is still offered.

`--no-lzma2` saves 1.9 s and produces identical bytes: the Tier-1 fallback trial ran, lost,
and cost 7% of the run. That is the price of the guarantee, and it is on the record here
rather than assumed.

## The inflate ceiling, and the one that did not work

The transform had no cap on how far an IDAT stream may inflate. The other container
wraps (`zstd_wrap`, `xz_wrap`, `brotli_wrap`) cap **relative** to the input — 4096×,
floor 256 MiB — because those formats can inflate unboundedly from a tiny stream. Copying
that rule here produces a guard that can never fire:

| input | ratio cap (4096×) | most deflate can produce (≈1032:1) |
|---|---|---|
| 92 KB | 380 MB | 95 MB |
| 1 MB | 4 GiB | 1 GiB |
| 10 MB | 40 GiB | 10 GiB |

Deflate's own maximum expansion is about **1032:1**, so the ratio branch is unreachable,
and the 256 MiB floor is unreachable below 64 KiB. A ratio cap on a PNG is decoration.

What actually costs time is the **absolute** size, because the engine compresses inflated
pixels at ~131 KB/s. So the ceiling is absolute:

```
kPngMaxInflated = 256 MiB    // ≈ 33 minutes at 131 KB/s
```

Past it the transform declines and the PNG is compressed as opaque bytes. A 93 KB
screenshot (3.4 MB inflated) is three orders of magnitude clear of it, and the archive
for `docs/shot.png` is unchanged at 67,217 bytes with a byte-exact round trip.

The distinction matters for the next container someone adds: **cap by ratio when the
format can inflate without bound; cap by absolute size when the cost is the size.** PNG
is the second kind, and the note in `png_wrap.hpp` says so where the constant lives.

## Filters: the engine wants the opposite of what deflate wants (2026-11-18)

Every PNG row carries a filter byte, and the row's bytes are *deltas* against the row
above or the pixel to the left. That is exactly right for deflate — it turns a gradient
into small numbers — and it is what this engine has always compressed, because the
transform inflates the IDAT and hands the bytes over as they are.

A context-mixing model does not need the deltas. It predicts from context, and a
screenshot's rows repeat exactly once the filters are removed. Measured by taking the
same image, unfiltering it to true pixels in Python, and compressing both streams with
`ziptastic -9`:

| image | filtered (today) | unfiltered + filter map | change |
|---|---|---|---|
| `docs/shot.png` — 1,280×900 UI screenshot, flat colour | 60,678 | **44,694** | **−26.3%** |
| `portrait.jpg` converted to PNG — 320×320 photograph | **65,152** | 97,556 | **+49.7%** |

(The filter map is 900 bytes for the screenshot — one byte per row — and 229 compressed.)

**Both directions, and by a lot.** A screenshot is the best case for unfiltering: 642 of
its 900 rows use filter 2 (Up), which encodes "same as the row above", and removing that
turns whole rows into runs the model predicts. A photograph is the worst case: adjacent
pixels differ smoothly, the deltas are small, and the filtered stream is what compresses.

So the answer is not "unfilter PNGs". It is **choose per file**, and the cost of choosing
badly is 50% — which means the choice cannot be a constant.

### What this changes about "filter search"

The plan item was "re-choose the per-row filter before compressing", which is what
optipng, zopflipng and ECT do. Their objective is the smallest *deflate* stream, and they
search filter strategies to get it. That objective is not this engine's: here the encoder
is a context-mixing block loop, and on a screenshot it does better with no filters at all
than with the best filter set deflate could ask for.

So the useful transform is:

1. inflate the IDAT, split each row into its filter byte and its deltas;
2. unfilter to true pixels, keep the filter bytes as a side array (one per row);
3. **decide by measurement, on a sample** — the same 256 KiB sample trial the rest of this
   engine uses — whether the unfiltered stream or the filtered one is smaller;
4. store the winner, and on decode refilter with the stored bytes so the original IDAT is
   reproduced exactly before preflate's diff is applied.

Step 3 is the whole design. Without it, half the corpus gets 50% worse; with it, the
screenshot case gains 26% and the photographic case is untouched. And the decision has to
be a *sample* measurement, not a full second pass: the full pass costs 25 s and the sample
costs about 1.3 s at the engine's measured 200 KB/s.

**Implemented, trial-gated.** `png_unpack_unfiltered()` produces the second form; the PNG
stage in `compressor.cpp` builds both and lets `precomp_helps()` — the same 256 KiB sample
trial the rest of the chain uses — choose. Nothing is decided by a constant.

End to end, same harness, `ziptastic 1.4.3` against this build:

| image | 1.4.3 | with the choice | change | time |
|---|---|---|---|---|
| `docs/shot.png` — 1,280×900 screenshot | 67,217 | **51,822** | **−22.9%** | 18.7 → 44.1 s |
| a 320×320 photograph as PNG | 70,322 | 70,308 | −0.02% (tie) | 1.4 → 2.3 s |

The photograph is unharmed because the trial picks the filtered form for it — which is the
whole point of measuring rather than choosing. Both round-trip byte-exact.

**The cost is time: +136% on the screenshot**, because two payloads are built and sampled
before one is kept. The payload builds are cheap — 208 ms and 179 ms for the two forms of a
3.4 MB image — so the cost was the *trial*: `precomp_helps` codes both streams in full below
8 MiB, which is two 3.4 MB encodes. Passing it a 256 KiB sample budget instead:

| trial | screenshot | photograph | time |
|---|---|---|---|
| full encode of both (8 MiB rule) | 51,822 ✓ | 70,308 ✓ | 44.1 s |
| 64 KiB sample | **67,222 ✗** | 70,308 | — |
| 256 KiB sample | 51,822 ✓ | 70,308 ✓ | **29.2 s** |

64 KiB was tried first and **chose wrong** — the middle 64 KiB of this payload is a busy
region where the two forms look alike. 256 KiB resolves it and costs 15 s less than the
full encode. That is the sampling floor for this decision on this content; a smaller one
would need a different sample location, not just a smaller one. That is the honest trade and it is the next thing to attack — the
sample could decide on a fraction of the rows, or a cheap statistic could pre-filter the
cases where unfiltering cannot win. Neither is written.

Scope: 8-bit images only (colour types 0/2/3/4/6). Sub-byte and 16-bit depths keep the
filtered form, where the row geometry is unambiguous; anything else would be a guess.
Payloads carry a version marker, so archives written before this still decode — the
repack path accepts both forms, and `the_unfiltered_mode_is_byte_exact` asserts both.
