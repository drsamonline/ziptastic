# GIF one level down: the same scan, and the same class of bug

Written 2026-11-03, the third of the nested-image series after `PNG_NESTED.md` and
`JPEG_NESTED.md`.

## Result

| case | before | after | gain | `layers2` |
|---|---|---|---|---|
| ZIP of four GIFs (one animated, three still) | 11,931 | **3,626** | **−69.6%** | 0x08 |

Round trip byte-exact. Before the nested stage worked, the same ZIP compressed to 5,872 — the
gain there came from precomp alone, and the GIF transform adds another **38%** on top.

## What changed

`gif_pack` no longer requires `GIF8` at offset 0: it scans for the signature, parses the GIF
from there, and carries the bytes in front of it as a prefix. The payload gains a leading
`prefix_len | prefix`, and the skeleton now starts at that offset rather than at zero.

## The bug, and why it is the third of its kind

`gif_unpack` rebuilt the file starting its logical position at **0**, but the recorded offsets
are absolute in the original buffer, so with a prefix present it copied `prefix_len` bytes too
many from a skeleton that does not contain them. The rebuild disagreed with the input, the
stage's verify-by-rebuild declined, and the transform silently did nothing — 0.0% with no error
anywhere.

That is the third payload-layout bug in this series:

| transform | the bug | why it hid |
|---|---|---|
| JPEG | tail written before the encoded image, read as "all but the last tail_len" | identical when the tail is empty — i.e. the standalone case |
| PNG | payload had no prefix concept at all | the standalone case has no prefix |
| GIF | logical position started at 0 instead of at the prefix | the standalone case has no prefix |

**Every one of them is invisible without a prefix or a tail, and every one of them appeared the
moment the transform was used on a container payload.** The pattern is worth naming: a
single-file transform that grows a prefix/tail to handle containers has a whole class of bugs
that only exist in the new case, and the existing tests all exercise the old one.

The rule that follows: **when a format grows an optional section, add a test where the section
is non-empty in the same commit**, or the empty case silently defines the behaviour.

## Where the nested-image series stands

| container | transform | measured |
|---|---|---|
| ZIP of PNGs | inflate PNG's DEFLATE | **−79.2%** |
| ZIP of GIFs | decode LZW to indices | **−69.6%** |
| ZIP of JPEGs | brunsli on every image | **−16.5%** |

All three scan for their signature anywhere in the payload, all three are multi-image, all
three round-trip exactly.

## What is left

**The precomp gate**, still the one structural item: `precomp_helps` decides whether to keep the
inflated payload before the nested transforms have had a chance to act on it. A member that
only pays *after* a nested transform can still be rejected upstream — which is why a ZIP of
JPEGs whose members are individually incompressible can lose the brunsli gain entirely.
