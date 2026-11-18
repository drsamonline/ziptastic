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

## The speed/ratio frontier a caller can use today (2026-11-18)

Everything above is one point on a curve. Measured on `docs/shot.png` with the unfilter
trial in place, so these are the choices available now:

| run | bytes | time | vs default |
|---|---|---|---|
| `-9` | **51,822** | 17.2 s | — |
| `-9 --no-parse` | 52,884 | **12.6 s** | +2.1% size, **−27% time** |
| `-9 --no-lzma2` | 51,822 | 17.2 s | nothing: the LZMA2 trial ran and lost |
| `-9 --no-bwt` | 51,822 | 17.0 s | nothing: BWT ran and lost on every block |
| `-9 --no-precomp` | 86,626 | 0.5 s | +67% size, −97% time |

Re-measured 2026-11-18 after the trial moved to a 64 KiB pixel-stream sample, so this table and
the one above it are not comparable — the default got 1.4 s faster and 15,395 bytes smaller
between them. Against 1.4.3 the default is now **67,217 → 51,822 bytes**, and never slower across three
interleaved runs per binary (20.5/20.5/23.7 s → 18.8/19.1/19.9 s; §0 says this box has no
stable clock, so single-pair timings are not quoted).

**`--no-parse` is the good speed point now.** It costs 2.1% and saves 22%, where it used to
cost 4.6% on the filtered stream: with the unfiltered payload the parse has much less to do,
because the rows are long runs the model predicts without searching.

**The two flags together store the file.** 92,930 bytes against a 92,822-byte input is not a
regression: the framing overhead is exactly 108 bytes, measured on incompressible input
(`/dev/urandom`, 92,822 → 92,930). So `--no-parse --no-lzma2` makes the engine give up and
store, which is the guard doing its job — but a caller reading the number without the framing
constant would think the archive grew.

**What this does not give you.** No point on this curve is fast *and* small: the only
sub-second option is 67% larger, and the only small option is 30 seconds. That is what S1 in
the plan is for — a fast deflate re-encode of the IDAT, the way ECT and oxipng do it, which is
a second code path and not a flag on this one.

## The fast point S1 is aiming at (2026-11-18)

The frontier above has nothing between 0.5 s and 17 s. S1 was "a fast deflate re-encode of the
IDAT"; measured with zlib at level 9, on the same screenshot, to see whether that is worth
building:

| stream | zlib-9 | time | throughput |
|---|---|---|---|
| the file's own filtered bytes | 91,773 | 0.15 s | 24 MB/s |
| **unfiltered pixels** | **70,987** | **0.11 s** | 30 MB/s |

Three things fall out:

1. **Deflate on the filtered bytes reproduces the file** — 91,773 against a 92,822-byte PNG. So a
   fast path that simply re-deflates what is there is worth nothing, which is the trap S1 would
   have walked into if it had been built before measuring.
2. **Deflate on the unfiltered pixels is worth 23%** on its own, in a tenth of a second. That is
   the same lever as §6i, applied to a different encoder: the deltas exist for deflate, but a
   screenshot's rows are long runs and even deflate prefers them.
3. **The fast point lands at roughly 72 KB** (70,987 plus the skeleton, the preflate diff and
   framing) **in about 0.2 s** — against 86,626 in 0.5 s for `--no-precomp` and 51,822 in 17 s for
   the CM path. That is the missing middle, and it is 100× faster than the CM path for 39% more
   bytes.

So S1's design is now specific rather than a direction: keep preflate's diff (it is what makes
the output exact), replace the CM pass over the pixels with a deflate of the *unfiltered* stream,
and store the filter map so decode can refilter. A zopfli-class encoder — ECT's, or the
`zopfli-master` already in the vault — would trade seconds for bytes on top of this, and the
numbers above say where that curve starts.

Not implemented. zlib's numbers are used because zlib is already linked; zopfli and libdeflate
are in the vault and would need a build target.

## S1 shipped: the fast form (2026-11-18)

`--png-fast` compresses the image's pixels **inside the transform** with zstd, so the block loop
has nothing left to do on the 3.4 MB that make the default slow. Measured, `-9`, `docs/shot.png`,
all byte-exact:

| run | bytes | time |
|---|---|---|
| `-9` (CM over the pixels) | **51,822** | 17.6 s |
| **`-9 --png-fast`** | **62,618** | **1.3 s** |
| `-9 --no-precomp` | 86,626 | 0.5 s |
| 1.4.3, for reference | 67,217 | ~18–20 s |

**62,618 bytes in 1.3 s**: 7% smaller than 1.4.3 in a fourteenth of the time, 28% smaller than
`--no-precomp` for 0.8 s more, and 21% larger than the CM default for 13× less time. The
predicted ≈72 KB was pessimistic — zstd at level 9 beats the zlib-9 measurement this section
started from.

Three forms now exist, and all three round-trip byte-exact (`test_png`):

| form | payload | what it is |
|---|---|---|
| plain | 3,462,638 | the file's own deltas, handed to the block loop |
| unfiltered | 3,462,640 | true pixels, handed to the block loop |
| **fast** | **65,856** | true pixels, zstd-compressed inside the transform |

The flag is a choice, not a default: a caller who has not asked for speed should get the ratio.
`png_unpack_fast` and the payload's second flag bit are the whole mechanism; the decode side
decompresses, refilters with the stored filter bytes, and preflate's diff makes the original
IDAT exact as before.

## A four-class corpus (2026-11-18)

Every number above was one file. Measured with the **shipped 1.4.4 binary** on four PNG classes
plus a real PDF with an embedded JPEG:

| file | class | raw | `-9` | `--png-fast` | `--no-precomp` |
|---|---|---|---|---|---|
| `ui.png` | 1,280×900 UI screenshot, flat colour | 92,822 | **51,822** (24.4 s) | **62,618** (1.4 s) | 86,626 |
| `photo.png` | 400×400 photograph, smooth gradients | 116,378 | **97,504** (4.0 s) | 115,774 | 115,774 |
| `palette.png` | 300×300, colour type 3 (palette) | 16,927 | **13,831** (1.0 s) | 17,035 | 17,035 |
| `diagram.png` | 900×600 line art, mostly white | 4,491 | **938** (5.1 s) | 2,155 | 2,155 |
| `photo.pdf` | a real PDF, `/DCTDecode` JPEG inside | 24,387 | **17,594** (0.3 s) | 17,594 | 17,594 |

Two things this settles and one it exposes.

**The headline is class-specific, as suspected.** The default's gain over storing the file is
−44% on the screenshot, −16% on the photograph, −18% on the palette image, −79% on the small
diagram, and −28% on the PDF. The −22.9% figure this file has been quoting is the *screenshot*;
quoting it unqualified was a one-file claim doing the work of a general one.

**The PNG layer fires on all four classes** — `layers2 0x10` in every default-mode archive. The
transform is not screenshot-only; only the *size* of its win is.

**`--png-fast` fires on one of the four.** On the other three its archive is byte-identical to
`--no-precomp`, which is the Tier-0 guard at work: the fast form compresses the pixels with zstd
and hands an already-compressed stream to the block loop, and where unfiltering buys little the
result is no better than storing the file, so the guard keeps the stored form and the layer flag
clears. That is correct behaviour — but it means the changelog line "13× faster for 21% more
bytes" is a *screenshot* statement. On a photograph `--png-fast` is neither faster nor smaller
than `--no-precomp`; it simply declines.

So the honest summary of the fast form: it is for flat-colour images, where unfiltering plus a
fast deflate beats the file's own DEFLATE. Everywhere else the engine stores, and did before.

## zopfli on the fast form: measured and rejected (2026-11-18)

The plan said a zopfli-class encoder would "move the 62,618 / 1.3 s point left on bytes without
giving back the time". Measured, with the vault's own `zopfli-master` built against the same
unfiltered pixel stream the fast form uses:

| encoder | bytes | time | throughput |
|---|---|---|---|
| zopfli, 1 iteration | 64,905 | **16.8 s** | 0.21 MB/s |
| zopfli, 5 iterations (256 KiB sample) | 4,278 | 2.6 s | 0.10 MB/s |
| zopfli, 15 iterations (256 KiB sample) | 4,276 | 5.6 s | 0.05 MB/s |
| **zstd -9, the fast form as shipped** | **65,856 for the whole payload** | **1.3 s** | 2.6 MB/s |

Two things kill it:

1. **zopfli's 64,905 is the pixel stream alone.** The entire zstd payload — pixels, filter map,
   preflate diff and skeleton — is 65,856. There is no size to win.
2. **It is 13× slower.** 16.8 s is the CM default's own wall-clock, with a 25% larger result. So
   zopfli is strictly dominated by *both* points on the frontier: slower than the fast form and
   bigger than the default.

**Iteration count is not the lever either.** On a 256 KiB sample, 1 → 15 iterations buys **0.5%**
for 3.7× the time, so no amount of waiting closes a 20% gap. That is worth knowing before anyone
tries the same thing with more patience.

This is the second time measuring before building has removed a plan item rather than implemented
one — the first was the original S1, whose obvious version (re-deflate the file's own bytes)
reproduced the file. Both are recorded where the next person will read them.
