> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# PNG one level down: the bit is in, the gain is not — and why

Written 2026-11-03. `kLayer2PngNested` (layers2 bit 7, the last free bit) is implemented and
wired. On the case it was built for it measures **0.0%**, and the reason is worth more than
the number.

## The measurement that motivated it

A ZIP of six 256×256 PNGs:

| | bytes |
|---|---|
| the ZIP | 126,175 |
| compressed today | **119,051** |
| compressed from the PNGs' inflated scanlines (the ideal) | **23,969** |

**−79.9%.** A ZIP of PNGs is two compression layers deep — the PNG's DEFLATE inside the ZIP's
DEFLATE — and the archive stores both.

## What actually happens

With the stage wired in, the same ZIP compresses to **119,051 — unchanged**, `layers2 = 0x00`.

`flags = 0x05` confirms **precomp did fire**: it inflated the members and produced a payload of
six concatenated PNGs plus its recipe. The nested PNG stage then declined, because
`png_unpack` walks chunks from offset 8 and requires the walk to consume the payload exactly —
which is right for one PNG and false for six.

So the stage is correct and does nothing, for a structural reason: **the payload a ZIP produces
is not one PNG, it is several, and the transform only knows how to be a single-file transform.**

## What the real fix looks like

Two options, and the second is better:

1. **Loop over consecutive PNGs** in the payload — walk PNGs until the bytes run out, store one
   structure per image, and rebuild them in order. Mechanical, and it turns the 79.9% into
   something real for any archive of loose PNGs.
2. **Let the nested stages run before the gate that judges them.** The deeper problem is that
   `precomp_helps` decides whether to keep the inflated payload *before* the nested transforms
   have had a chance to act on it, so it compares "inflated member bytes" against "the ZIP",
   when the honest comparison is "what the whole chain would produce". That gate is what makes
   a ZIP of PNGs look like a loss to precomp in the first place.

Option 1 is the smaller change and captures the measured win; option 2 is the more correct one
and would help every nested transform, not just PNG.

## Status

The bit is taken, the stage is wired, the round trip is exact, and the measurement is recorded
rather than quietly dropped. **Not worth keeping without option 1 or 2** — and `layers2` is now
full, so whatever comes next needs a third layer byte.


## Second attempt: the multi-image payload, and the real diagnosis

The first write-up blamed "six concatenated PNGs" and proposed looping over them. That was
wrong. The transform now handles a sequence of consecutive PNGs, and it works at the top
level — six concatenated PNGs (156,128 bytes) compress to **24,232** with `layers2 = 0x10`,
round-tripping exactly. The nested case still measures 0.0%.

**The real reason is the payload's layout, not its content.** `preflate_unpack` builds a
payload that starts with a **stream count varint**, then per-stream headers, then the
correction bytes, then the inflated member data, then the container skeleton. So the PNG
bytes are in the *middle* of the payload, and `png_likely` — which checks the PNG signature at
offset 0 — is false for every precomp payload, always.

That is a structural limit of the whole nested-transform idea, not of PNG:

- `jpeg_nested` and `gif_nested` fire only because **base64** runs first and produces a
  payload that *is* the blob, starting at offset 0.
- Anything reached through precomp sits behind a container-specific header, and a
  single-file transform cannot see it.

**The fix is not another loop.** It is to let the nested stages operate on the **inflated
section** of the precomp payload rather than on the payload as a whole — carry the recipe
alongside instead of in front. That is a change to the recursion's contract, and it would make
every nested transform (PNG, JPEG, GIF, and anything added later) work on container members
instead of only on base64-decoded blobs.

## Status, corrected

The bit is taken and the stage is wired, but it **cannot fire on the case it was built for**,
and neither can any other image stage behind precomp. The measurement stands: a ZIP of six
PNGs is 119,051 bytes today against an ideal of 23,969, and the gap is the recursion's payload
contract rather than PNG handling.


## Third pass: the anywhere-scan, and it works

The fix turned out to be the one this project already uses for archives inside tar: **do not
require the signature at offset 0.** `png_unpack` now scans for the first PNG signature
anywhere in the payload, carries the bytes in front of it as a prefix, and finds consecutive
PNGs from there. `png_likely` agrees, and `scan_zip` is the precedent — it made exactly this
move for ZIPs inside a `.tar.gz` and gained 44.8%.

| case | before | after | gain |
|---|---|---|---|
| ZIP of six 256×256 PNGs | 119,051 | **24,750** | **−79.2%** |
| six concatenated PNGs, no container | 156,128 raw | 24,232 | — |

`layers2 = 0x80`, round trip byte-exact, 32/32 suites pass. The ideal measured at the start was
23,969; the shipped result is 24,750 — within 3%.

So the earlier conclusion — "the recursion's payload contract is the ceiling" — was right about
the *mechanism* and wrong about the *fix*. The contract did put the images in the middle; the
answer was to look in the middle rather than to change the contract.

**A false positive costs nothing**, which is why this is safe: `walk_one` validates every chunk
length and requires an IEND, and the stage rebuilds the input and byte-compares before the
payload is adopted. The same reasoning that made the ZIP anywhere-scan safe.

## What this means for the other nested stages

`jpeg_nested` and `gif_nested` still only look at offset 0, so they fire on base64-decoded
payloads and not on container members. The same scan would work for both — JPEG is `FF D8 FF`
and GIF is `GIF87a`/`GIF89a`, both far weaker signatures than PNG's eight bytes, so a scan for
them needs the verify-by-rebuild to do more of the work. That is the next piece, and it is now
a known shape rather than an open question.
