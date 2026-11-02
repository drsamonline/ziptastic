# base64 one level down — shipped, and it is worth 0.7% not 19.5%

Written 2026-11-02. Layer bit `kLayer2Base64Nested` (layers2 bit 1).

## What it does

The recursion already re-entered precomp and the numeric/XML transforms on what a
container inflated. It now also tries base64, which is the shape a `.json.gz` has:
gzip inflates to a document, and the document's payload is a base64 blob that nothing
was decoding.

## Measured

| file | without | with | gain | `layers`/`layers2` |
|---|---|---|---|---|
| `photo.json` (base64 of a JPEG, bare) | 21,564 | **17,356** | **−19.5%** | 0x10 / 0x00 |
| `photo.json.gz` (the same, gzipped) | 21,688 | **17,414** | **−19.7%** | 0x20 / 0x06 |

**Update, same day:** the 0.7% was the first version. The nested chain re-entered
precomp, base64 and the numeric/XML transforms but *not* the image transforms, so a
decoded JPEG was handed to the backend as ordinary bytes. Adding nested JPEG and GIF
(`kLayer2JpegNested`, `kLayer2GifNested`) closes the gap: **−19.7%**, marginally better
than the same document uncompressed. The 0.7% figure is kept here because it is the
measurement that said where to look.

## Why the gap, and what it costs

The bare `photo.json` gains 19.5% because after base64 decodes, the **JPEG transform**
runs on the blob at offset 0 and brunsli re-encodes the photo. One level down, the
nested stage re-enters only **precomp, base64 and the numeric/XML transforms** — the
image transforms are not in the nested chain. So the blob is decoded and then handed to
the backend as ordinary bytes, which is worth almost nothing on already-compressed
pixels.

Closing it means adding nested JPEG and GIF stages, which needs `kLayer2JpegNested` and
`kLayer2GifNested` (both bits are free) plus the same mirror-order undo. That is a
follow-up, not a defect: what shipped is exact, gated, and positive — it just does not
collect the part of the win that lives in brunsli.

## What it is worth today

- A `.json.gz` holding a base64 **text** payload: the decode still removes the 33%
  inflation and the line wrapping, and the backend then sees the text directly.
- A `.json.gz` holding a base64 **image**: 0.7%, because brunsli does not run. The
  nested-image item is the one that pays.

## Files

- `include/chimera/format.hpp` — `kLayer2Base64Nested` (layers2 bit 1)
- `src/engine/compressor.cpp` — the stage, after the second container level and before
  the nested numeric transforms
- `src/engine/decompressor.cpp` — undone between them
- `tests/unit/test_containers.cpp` — `base64_inside_gzip_is_decoded`, asserting both
  `kLayerGzip` and `kLayer2Base64Nested` and an exact round trip

## Reproducing the fixture

```python
import base64, gzip, json
jpg = open('color_photo.jpg','rb').read()
doc = json.dumps({"name":"photo.jpg","encoding":"base64",
                  "data": base64.b64encode(jpg).decode()})
open('photo.json.gz','wb').write(gzip.compress(doc.encode(), 9))
```

`tests/unit/fixtures_gz_b64_jpeg.inc` holds this file as a byte array. It is 21 KB of
source on purpose: a smaller JPEG does not exercise the path, and the reason is worth
knowing — at 1 KB the gzip gate declines the whole chain before any of this runs, and
the trial has no signal to work with.
