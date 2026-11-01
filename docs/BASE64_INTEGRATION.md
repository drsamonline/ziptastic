# base64 as a Layer 0 stage — shipped, and the gate was the hard part

Written 2026-11-01. `docs/BASE64_MEASUREMENT.md` decided *whether* this was worth
building; this records what happened when it was built. Every number is from a run
in this session.

## Result, through the shipped CLI, all round-trip exact

| file | without | with | gain | `layers` | decision |
|---|---|---|---|---|---|
| base64 of a 21,797 B JPEG, single line | 21,481 | **17,291** | **−19.5%** | 0x10 | taken |
| the same, MIME-wrapped at 76 columns | 21,927 | **17,533** | **−20.0%** | 0x10 | taken |
| the same inside a JSON document | 21,551 | **17,349** | **−19.5%** | 0x10 | taken |
| base64 of 200 KB of spreadsheet XML, wrapped | 21,177 | **10,990** | **−48.1%** | 0x10 | taken |
| the same, single line | 10,282 | 10,282 | 0 | 0x00 | **declined** |
| the same inside JSON, single line | 10,363 | 10,363 | 0 | 0x00 | **declined** |

The two declines are the point. Base64 of structured text on one line compresses
*better* than the text does, so the transform loses 6.3% there; the same payload
wrapped at 76 columns wins 48.1%. The difference is the line breaks, not the 33%
inflation the earlier doc was written to correct.

## Two findings that changed the design

**The blob must sit at offset 0.** A first version put a small header in front of
the decoded bytes. brunsli then no longer recognised the JPEG inside, and the MIME
case returned 2.2% instead of 20.0%. The payload is now

```
blob | skeleton | recipe | u32 recipe_len | u32 blob_len | "B641"
```

parsed from the end, with the decoded bytes first so every downstream stage sees
what it would have seen if the user had handed over the binary file.

**A 32-character final line is not a 31-character run.** The minimum-run floor was
applied to the character count *excluding* padding, so the last line of every
MIME-wrapped file was rejected. That left the final 23 bytes of the blob behind —
enough that brunsli refused the truncated JPEG, and the whole MIME case came out at
2.2%. The floor now counts the padding: `core_len + pad`.

## The gate, and three wrong versions of it

The transform is applied through an accept/reject trial, not a rule. Getting that
trial right took three attempts, and each failure was the same mistake in a
different costume — measuring something other than what the engine would do:

1. **Range coder + BWT only.** Declined a JPEG worth 19.5%, because brunsli — which
   would actually fire — was not in the model.
2. **Adding JPEG, GIF and DEFLATE by hand.** Fixed the JPEG case, and still got the
   XML case backwards: the spreadsheet transform, which is most of what makes that
   payload cheap, was not in the model either.
3. **Running the real pipeline, with the LZMA2 fallback switched off.** On a 256 KiB
   prefix of the base64 text the native encoder costs 16,412 and LZMA2 costs 10,128,
   so the comparison made a clear 6% loss look like a tie and the transform was
   taken.

What ships now runs `compress()` itself on a 256 KiB sample of each side, with
base64 disabled so the trial cannot recurse and `--max-ratio` off so it does not pay
for the ratio ceiling. Everything else — every Layer 0 transform, the per-block
backend selection, the Tier-1 LZMA2 comparison — is the real thing.

256 KiB, not 64: at 64 KiB the trial flipped both XML cases back to "take". The
margin it has to resolve is 6%, and a small sample cannot resolve it.

## What it costs

| file | without the trial | with | overhead |
|---|---|---|---|
| `jpeg.mime.txt` (29 KB) | 657 ms | 1,780 ms | **+171%** |
| `xml.mime.txt` (270 KB) | 2,121 ms | 5,657 ms | **+167%** |

That is the price of a decision that is actually right, and it is paid only when a
long alphabet run exists *and* packing succeeded. `--no-base64` turns it off. It is
a lot, and the honest summary is: 48% smaller for 2.7× the time on a wrapped
payload, and 2.7× the time for nothing on the cases that decline. A cheaper gate
that is also correct would be a genuine improvement; three attempts suggest it is
not a small one.

## Two things this uncovered

**base64 fires on random binary.** Random bytes contain runs of alphabet
characters long enough to pass the pre-check, and sometimes the transform wins. It
is exact and gated, so this is not a bug — but it means the trial can run on files
that have nothing to do with base64.

**`original_size` describes the payload after Layer 0, not the caller's bytes.**
That has always been true — it is what the block loop is checked against, and the
CRC-32 of the true original is what guards the round trip — but nothing exercised
it until base64 started firing on random data, and `test_roundtrip` had been
asserting the two are equal. The assertion now allows them to differ when a Layer 0
transform ran. The format doc says "total byte count of the uncompressed input",
which was never quite what the code did.

## Not built

The **nested** variant — base64 inside a ZIP or a `.tar.gz`, which is where a JSON
API response inside a container would land. Layer bit 5 is free and reserved for
it in `format.hpp`'s comment, but the constant is deliberately not declared until
something sets it.

## Files

- `src/precompressors/base64_wrap.{hpp,cpp}` — the transform and its inverse.
- `include/chimera/format.hpp` — `kLayerBase64` (0x10) in `kLayerMask`; bit 5 free.
- `src/engine/compressor.cpp` — the stage, `actual_cost()` and `base64_pays()`.
- `src/engine/decompressor.cpp` — undone last, because it was applied first.
- `src/engine/compressor.hpp`, `src/main.cpp` — `opt.base64`, `--no-base64`.
- `tests/unit/test_base64.cpp` — 12 cases: single line, MIME, CRLF, stripped
  padding, URL-safe alphabet, embedded in a document, several runs, prose declined,
  short runs declined, damaged payload refused, determinism, blob-first layout.

## Reproducing

```bash
python3 -c "
import base64,json
raw=open('photo.jpg','rb').read()
s=base64.b64encode(raw).decode()
open('mime.txt','w').write('\n'.join(s[i:i+76] for i in range(0,len(s),76))+'\n')
open('doc.json','w').write(json.dumps({'data':s},indent=2))"
ziptastic c -9 -o mime.txt.zt mime.txt      # layers=0x10, round trip exact
ziptastic c -9 --no-base64 -o mime.off.zt mime.txt   # the comparison
```
