> **Superseded for current figures by [`FINDINGS.md`](FINDINGS.md).** This document is the
> working record — how the number was arrived at and what was corrected along the way. Where the
> two disagree, `FINDINGS.md` is right.

# JPEG one level down: the anywhere-scan, and the ordering bug it exposed

Written 2026-11-03, the follow-up to `PNG_NESTED.md`.

## Result

| case | before | after | gain | `layers2` |
|---|---|---|---|---|
| ZIP containing one 256×256 JPEG | 31,032 | **26,200** | **−15.6%** | 0x04 |
| ZIP containing four JPEGs | 125,811 | **105,064** | **−16.5%** | 0x04 |

**Both cases now transform every image.** The first version handled only the first JPEG in the
payload, which measured −5.1% on the four-image ZIP; the multi-image version takes it to
**−16.5%**, and the single-image case is unchanged at −15.6%. A ZIP of many JPEGs is the
game-asset case, so the difference mattered.

## What changed

`jpeg_pack` no longer requires the signature at offset 0. It scans for the first SOI anywhere,
then walks EOI candidates until brunsli accepts one, and stores the bytes before and after the
image as a prefix and a tail. Same shape as the PNG anywhere-scan, which was worth 79.2% on a
ZIP of PNGs.

The payload is `[prefix_len][prefix][tail_len][tail][brunsli]`.

## Two bugs, both found by the tests rather than by reasoning

**1. `jpeg_likely` changed meaning by accident.** It used to be a signature test — "does this
start with SOI and a marker" — and the scan version made it require a complete image with an
EOI. `detects_jpeg_headers` caught it: a five-byte header fragment is still jpeg-ish, and the
gate should say so. `jpeg_likely` is back to a permissive signature check (anywhere), and
whether a complete image can be *delimited* is `jpeg_pack`'s problem.

**2. The tail was in the wrong place.** `jpeg_unpack` read the encoded image as "everything
except the last `tail_len` bytes", but the payload writes the tail *before* the encoded image.
With an empty tail — the standalone case — both readings agree, so every existing test passed.
It only broke when the tail was non-empty, which is exactly the nested case the scan was added
for. The nested JPEG test failed, the round-trip test passed, and the difference between them
was the bug.

That is the second time this session a payload layout error hid behind a case where two
interpretations coincide. The lesson is not "be careful"; it is that **a format with an optional
section needs a test where the section is present**, or the absent case silently defines the
behaviour.

## What is left

- ~~Multi-image JPEG~~ **DONE** — every image in the payload is transformed, −16.5% on the
  four-image ZIP against −5.1% for the first-only version.
- **The same scan for GIF** (`GIF87a`/`GIF89a`), which is a four-byte signature and will need
  the verify-by-rebuild to do most of the work.
- **The precomp gate**, still the deeper issue: `precomp_helps` decides whether to keep the
  inflated payload before the nested transforms have had a chance to act on it, so a member
  that only pays *after* a nested transform can still be rejected upstream.
