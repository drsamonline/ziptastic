# SQLite pages grouped by type — shipped

Written 2026-11-02. Measured in Python before any C++ was written, then again through
the shipped CLI.

## Result

| file | before | after | gain | `layers2` |
|---|---|---|---|---|
| 295 KB database, 72 pages, 4 KiB | 119,016 | **113,272** | **−4.8%** | 0x01 |
| 3.46 MB database, 845 pages, 4 KiB | 1,468,255 | **1,375,283** | **−6.3%** | 0x01 |

Round-trip exact. This is the first user of the second `layers` byte added in v5.

## What the transform does

A database is a run of fixed-size pages of five kinds: interior index, interior
table, leaf index, leaf table, and everything else (freelist, overflow, lock bytes).
Interleaved, they make the backend keep switching statistics. The transform emits
them grouped by kind, stable within a kind, and stores the permutation.

Measured alternatives that did **not** ship:

- **Byte-plane split** of the page headers (all first-16-bytes together, then the
  bodies): −0.0% on the small database, +0.9% on the big one. No.
- **Zero side information** — the first design tried to have the decoder recompute the
  order from the page type bytes, which would have made the recipe 12 bytes. It does
  not work, and the reason is worth keeping: grouping is a sort by type, and the
  original indices of one type are scattered through the file, so "rank within type"
  is not the original position. The verify caught it immediately (payload decoded to
  the wrong bytes at offset 0). The permutation costs about 2 bytes per page and is
  kept.

## Two things this cost

**The usual "packed must be smaller" rule silently disabled it.** Every other
transform ends with `return packed.size() < in.size()`. A reordering plus a
permutation is *larger* on disk by construction — the gain only appears after the
backend runs — so the first version declined every database and reported 0.0%. The
check is removed here, with a comment saying why.

**A test fixture destroyed its own magic.** The synthetic database in
`test_sqlite.cpp` filled each page with distinct content starting at byte 8, which on
page 1 overwrites the 16-byte header. The transform correctly declined it and the
test read as a transform bug.

## Honest note on the size of the win

5–6% is small by this project's standards — the container transforms are 20–80%. It
ships because a database is a file class that had nothing, and 6% on a save file is
6%. It is also cheap: no parsing beyond the header, a 2-byte-per-page permutation, and
no risk to the payload's own bytes.

## Files

- `src/precompressors/sqlite_wrap.{hpp,cpp}`
- `include/chimera/format.hpp` — `kLayer2Sqlite` (layers2 bit 0)
- `src/engine/compressor.cpp`, `decompressor.cpp` — stage and inverse
- `src/engine/compressor.hpp`, `src/main.cpp` — `opt.sqlite`, `--no-sqlite`
- `tests/unit/test_sqlite.cpp` — 6 cases with a synthetic database

## Column filters: the opportunity is large and NOT YET PROVEN (2026-11-18)

The ClickHouse survey proposed a column-aware encoding under the codec, and Blosc2 (BSD) ships
exactly that as two filters — `shuffle` (transpose by element width) and `bytedelta` (delta within
each byte-channel). `scripts/column_filter_probe.py` measures both against plain zstd on a real
database's integer columns.

### The result, and why it does not count yet

| database | integer payload | zstd raw | +shuffle | +shuffle+bytedelta |
|---|---|---|---|---|
| `multi_big.db` (10 columns, 200,000 values) | 800,000 B | 48,306 | 993 | **411** |
| `multi_small.db` (8 columns, 9,600 values) | 38,400 B | 2,449 | 459 | **159** |

A 99% saving. **It does not count**, because both fixtures are formula-generated: sequential `id`
values and a computed `val`. Formula data makes any filter look miraculous — it is the same trap that
once made BMP read −99% and mean −37%.

I searched this environment for a real SQLite database to measure instead and found none: the Debian
`avahi/service-types.db` is not a SQLite file (`file is not a database`), and the one from lexora's
own test suite has **8 bytes** of integer data in total.

### What would settle it

One real database with real integer columns — ids that are not sequential, timestamps, prices,
foreign keys — run through:

```bash
scripts/column_filter_probe.py /path/to/real.db
```

If the filtered column is not clearly smaller than the raw one, the transform is not worth its
complexity and its byte-exact inverse. If it is, the design is: shuffle + bytedelta per column,
stored as a new stage inside the existing SQLite transform, with the filters re-derived from
Blosc2's BSD-2-Clause implementations and the inverse proven byte-exact on the corpus.

**Until that number exists, this transform is unstarted by choice, not by oversight.** The 27.8%
page-grouping result in this document stands on its own and is unaffected.
