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
