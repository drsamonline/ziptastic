# XML-aware numeric transform — measured, shipped, and worth −50.3%

Written 2026-11-01. Every number here came from a run in this session on the
machine described in `BENCHMARKS.md`; nothing is extrapolated.

## Why this existed

Recursive container descent works: precomp inflates an `.xlsx` and the engine
scores the inflated payload instead of the compressed container. What it could not
do was transform what came out, because a spreadsheet is not delimited text:

```xml
<row r="1"><c r="A1"><v>1003</v></c><c r="B1"><v>500123</v></c></row>
```

The CSV transform needs a consistent delimiter count per row and declines this.
So the nested numeric transform correctly did not fire, and the −50.7% recursion
result sat next to a −89.4% figure that needed cell values to be reachable. This
item closes that gap.

## Result, through the shipped CLI, all round-trip exact

| file | before | after | gain | `flags` | `layers` |
|---|---|---|---|---|---|
| `book.xlsx` (2 cols × 40,000 ints) | 147,525 | **73,367** | **−50.3%** | 0x05 | 0x08 |
| `sheet1.xml` (the inflated payload, alone) | 148,615 | **72,464** | **−51.2%** | 0x01 | 0x04 |
| sheet of random 9-digit integers | 495,648 | 414,446 | −16.4% | 0x01 | 0x04 |
| sheet of floats and strings | 196,987 | 196,987 | declines | 0x01 | 0x00 |
| `book.tar.gz` wrapping the xlsx | 208,652 | 208,652 | unchanged | 0x01 | 0x00 |

`layers` 0x04 is `kLayerXmlNumeric`, 0x08 is `kLayerXmlNested`. The xlsx figure is
the nested path: precomp inflates, the XML transform runs on the payload, and the
whole chain round-trips byte-exactly.

The `.tar.gz` row is the honest gap: precomp does not reach inside a **gzip**
container, so recursion never starts and no Layer 0 transform can fire. That is
item 6 in `SESSION_HANDOFF.md` §6 (gzip / multi-member gzip), not this item.

The random-integer row is the one worth keeping in mind. Delta coding buys nothing
there — random values have no local structure — and the transform still gains
16.4%, because a 30-bit value is 4–5 varint bytes against 9–10 decimal characters.
That part of the win is base conversion, not prediction, which is why it survives
on incompressible content.

## What the measurements settled

Two design choices were made by measurement rather than argument, before any C++
was written.

**Column order: group or interleave?** Grouping the values into contiguous
per-column streams measured 71,964 against 71,963 for one interleaved stream with
per-column delta state. 0.04% — nothing. The interleaved form needs no column
grouping, no permutation and no second index, so it is what shipped.

**Is per-column state worth it at all?** A flat single-prev variant, ignoring
columns entirely, also compresses to 71,994. But its intermediate stream is
239,999 bytes against 80,003, three times the data for the backend to capture. On
this fixture the backend absorbs it; on less regular content that is a ratio risk
with no upside, so per-column state stays.

**Per-cell decline, not per-column.** A cell that is a float, a string, `"007"`,
`"+5"` or `"1.2E5"` keeps its raw bytes in the skeleton and the rest of the sheet
still transforms. The CSV transform has to decline a whole column; this does not
have to, and the float sheet above shows the cost of not needing to.

## Format

```
"XN1" | varint runs | varint skeleton_len | skeleton | stream
```

`skeleton` is the input with the digits of every accepted `<v>` removed, so each
accepted cell reads `<v></v>`; every other byte is identical. `stream` holds one
zigzag varint per accepted cell, in document order, of the delta from the previous
value in that same column. The decoder scans the skeleton for empty `<v></v>`
placeholders and refills them in order — the same scanner derives the column ids
on both sides, from the `r="A1"` attribute when present and from the cell's ordinal
position in its row when it is not.

Two guards make the refill unambiguous: a buffer that already contains a literal
`<v></v>` is declined outright, and the compressor byte-compares
`unpack(pack(x)) == x` before accepting, at the top level and at depth 1.

## Three defects found while building it

**The decoder refused its own output.** It counted every `<c><v>` in the skeleton
as a slot to fill, including cells whose digits were declined and still sat there
raw. `slots != runs` and the decode failed. The unit test
`non_canonical_cells_stay_raw_and_still_round_trip` caught it before it ran
anywhere.

**The canonical-integer parser accepted `"-0"`.** It parsed to 0 and
re-serialised as `"0"`, losing the sign. This hole was already in the **CSV**
transform, where only the compressor's round-trip check kept it from shipping — the
whole CSV would have been declined rather than corrupted, which is why nobody saw
it. Fixed once in `src/precompressors/numeric_common.hpp`, which both transforms
now share, so the rule cannot drift between them.

**The stage-0 payload flag did not cover the new variant.** The engine chooses the
bytes every later stage codes with `numeric ? numeric_payload : input`, and the new
transform set only its own flag. Result: `layers` claimed the XML transform while
precomp, GIF and BCJ were handed the raw XML — every bare `sheet1.xml` compressed
and then refused to decompress. **No unit test could have caught this**; it lives
in the orchestrator. It surfaced by measuring through the CLI and reading the
`layers` byte, which is rule 4 earning its place for the fifth time.

That third one is the useful lesson: the transform was right, the tests were
right, and the bug was in which bytes the pipeline was told to carry.

## Files

- `src/precompressors/xml_numeric_wrap.{hpp,cpp}` — the transform and its inverse.
- `src/precompressors/numeric_common.hpp` — varint, zigzag and the canonical
  integer rule, shared with `numeric_wrap`.
- `include/chimera/format.hpp` — `kLayerXmlNumeric` (0x04), `kLayerXmlNested`
  (0x08), both added to `kLayerMask`.
- `src/engine/compressor.cpp` — top-level attempt before the CSV transform, and the
  nested attempt inside the recursion block; one `layers` value built once and
  used by all three encode paths.
- `src/engine/decompressor.cpp` — nested undone before `preflate_repack`,
  top-level after, mirroring the order they were applied.
- `tests/unit/test_xml_numeric.cpp` — 10 cases: round trips, ordinal-derived
  columns, negatives and zero, per-cell decline, float sheet declines, prose
  declines, empty `<v></v>` declines, per-column state, damaged payload refused,
  determinism.
- `tests/integration/test_cli.sh` — asserts the layer bit is set and the round trip
  is bit-exact through the binary.

## Reproducing

```bash
# 2 columns x 40,000 integers, zipped at level 9, ~524 KB
python3 - <<'EOF'
import zipfile
parts=['<?xml version="1.0"?><worksheet><sheetData>']
a,b=1000,500000
for i in range(1,40001):
    a+=3; b+=i%7
    parts.append('<row r="%d"><c r="A%d"><v>%d</v></c><c r="B%d"><v>%d</v></c></row>'%(i,i,a,i,b))
parts.append('</sheetData></worksheet>')
sheet=''.join(parts)
open('sheet1.xml','w').write(sheet)
with zipfile.ZipFile('book.xlsx','w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    z.writestr('xl/worksheets/sheet1.xml',sheet)
EOF
ziptastic c -9 -o book.xlsx.zt book.xlsx   # 73,367 bytes, flags=0x05 layers=0x08
ziptastic d -o book.xlsx.rt book.xlsx.zt && cmp book.xlsx book.xlsx.rt
```
