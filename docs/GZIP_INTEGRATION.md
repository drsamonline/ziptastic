# gzip as a Layer 0 container — shipped, and it unblocks the nested chain

Written 2026-11-02. Every number is from a run in this session.

## Result, through the shipped CLI, all round-trip exact

| file | without | with | gain | `layers` |
|---|---|---|---|---|
| `sheet.xml.gz` (3.25 MB of spreadsheet XML) | 74,776 | **58,888** | **−21.2%** | 0x28 |
| `logs.tar.gz` (6 text files) | 7,172 | **5,353** | **−25.4%** | 0x20 |
| `book.tar.gz` (an `.xlsx` inside a tar inside gzip) | 107,312 | **75,144** | **−30.0%** | 0x20 |
| `multi.gz` (two concatenated members) | 183 | 171 | −6.6% | 0x20 |

`layers` 0x20 is `kLayerGzip`; 0x28 is gzip **plus** the XML cell transform running on
what it inflated — the recursion working two levels deep on the first file.

The `book.tar.gz` row is the one that matters. Before this, a `.tar.gz` compressed to
**107,312 bytes against a 107,221-byte file** — precomp could not enter a gzip
container, so recursion never started and every Layer 0 transform was blind. That was
the blocker recorded in `SESSION_HANDOFF.md` §6 item 6, and it is gone.

## Why it was cheap to build

A gzip member is a 10-byte header, one bare DEFLATE stream, and an 8-byte
CRC-32/ISIZE trailer. The DEFLATE half is exactly what preflate already does for ZIP
members, so this is a container wrapper, not a new codec: `gzip_wrap.cpp` walks
members, keeps each header and trailer verbatim, and lets preflate produce the diff
and the inflated bytes. Nothing about DEFLATE parsing was reimplemented.

**The one thing that was not obvious: gzip does not store the compressed length.**
The end of a member's DEFLATE stream has to be found by parsing, and the trailer plus
the next member's header sit immediately after it. preflate's vector overload insists
on being handed exactly one stream and returns false otherwise — it decoded the stream
correctly and still said no. Its streaming overload reports `deflate_size`, which is
what makes multi-member files tractable at all. That is the whole reason this file
exists rather than being three lines around `preflate_unpack`.

## What is stored

```
"GZ01" | varint members | per member: header_len, diff_len, infl_len
       | all headers verbatim | all diffs | all inflated bytes | all 8-byte trailers
```

Headers and trailers are kept verbatim rather than reconstructed: mtime, the original
file name, extra fields and the OS byte are the caller's bytes, not ours to guess. The
payload is re-emitted and byte-compared against the input before it is returned, so a
member the transform misunderstood declines the whole file rather than transforming
part of it — `declines_trailing_rubbish` pins that.

Multi-member is not an edge case: `cat a.gz b.gz > all.gz` is how rotated logs are
kept, and the format requires a decoder to read every member.

## What it does not do yet

`book.tar.gz` gains 30%, not the −89.4% the ideal chain promised, because gzip hands
back a **tar** and the tar contains an **xlsx** — a ZIP. Reaching the sheet XML inside
needs one more level: a tar parser, then a second precomp pass. The recursion machinery
is already depth-generic; what is missing is the tar format. That is the next item, and
this document is its baseline.

## Files

- `src/precompressors/gzip_wrap.{hpp,cpp}` — the container. Built into the C++17
  `preflate_shim` target, because it includes preflate's C++14-era headers.
- `include/chimera/format.hpp` — `kLayerGzip` (0x20); bit 6 is free.
- `src/engine/compressor.cpp` — the stage, tried when precomp did not fire (a file is
  either ZIP/PDF or gzip, never both), and the recursion gate widened to `precomp || gzip`.
- `src/engine/decompressor.cpp` — undone next to precomp, after the nested transforms.
- `tests/unit/test_gzip.cpp` — 8 cases with the fixtures embedded as byte arrays, since
  the commit path is text-only and would silently skip a binary file.

## Reproducing

```bash
gzip -9 -c sheet.xml > sheet.xml.gz
ziptastic c -9 -o sheet.xml.gz.zt sheet.xml.gz          # layers=0x28, exact
ziptastic c -9 --no-precomp -o sheet.off.zt sheet.xml.gz
```
