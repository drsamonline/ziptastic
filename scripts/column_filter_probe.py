#!/usr/bin/env python3
"""column_filter_probe.py — would a column filter pay on THIS database?

The question, before any C++ is written: does a shuffle + byte-delta chain under
the codec beat the codec alone on the integer columns of a real SQLite file?

    scripts/column_filter_probe.py database.db [--level 19]

It reads every INTEGER column, packs it the way SQLite stores it (little-endian
4-byte), and compares zstd on the raw bytes against zstd after the two filters
Blosc2 applies (blosc/shuffle.c and plugins/filters/bytedelta/bytedelta.c):

    shuffle     transpose by element width, so each byte-position forms its own
                stream — the high bytes of every int end up together
    bytedelta   delta between consecutive elements within each stream

Why a script and not a transform: the last fixture this project measured a column
idea on was formula-generated, and formula data makes any filter look miraculous.
A fixture cannot answer this. A real database can, and this runs against one.

Read the number as: the share of the integer payload the codec still needs after
the filters. If the filtered column is not clearly smaller than the raw one, the
transform is not worth its complexity and its byte-exact inverse.
"""
from __future__ import annotations

import argparse
import sqlite3
import struct
import sys

try:
    import zstandard as zstd
except ImportError:
    sys.exit("needs zstandard: pip install zstandard")


def integer_columns(db: str) -> tuple[bytes, int, int]:
    """Every INTEGER column, packed as SQLite stores them."""
    con = sqlite3.connect(db)
    out = bytearray()
    ncols = 0
    nrows = 0
    for (table,) in con.execute(
        "select name from sqlite_master where type='table' and name not like 'sqlite_%'"
    ):
        for col in con.execute(f'pragma table_info("{table}")'):
            if not (col[2] or "").upper().startswith("INT"):
                continue
            ncols += 1
            for (value,) in con.execute(f'select "{col[1]}" from "{table}"'):
                out += struct.pack("<i", int(value or 0))
                nrows += 1
    con.close()
    return bytes(out), ncols, nrows


def shuffle(data: bytes, width: int = 4) -> bytes:
    n = len(data) // width
    out = bytearray(len(data))
    for i in range(width):
        out[i * n:(i + 1) * n] = data[i::width][:n]
    return bytes(out)


def bytedelta(data: bytes, width: int = 4) -> bytes:
    n = len(data) // width
    out = bytearray(len(data))
    for i in range(width):
        channel = data[i::width][:n]
        prev = 0
        for j, v in enumerate(channel):
            out[i * n + j] = (v - prev) & 0xFF
            prev = v
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("database")
    ap.add_argument("--level", type=int, default=19)
    args = ap.parse_args()

    raw, ncols, nrows = integer_columns(args.database)
    if not raw:
        print(f"{args.database}: no INTEGER column data — nothing to measure")
        return 2
    comp = zstd.ZstdCompressor(level=args.level)
    size = lambda b: len(comp.compress(b))
    shuffled = shuffle(raw)
    both = bytedelta(shuffled)

    print(f"{args.database}")
    print(f"  {ncols} integer columns, {nrows} values, {len(raw)} bytes of payload")
    print(f"  {'zstd(raw)':<24} {size(raw):>9}  {100*size(raw)/len(raw):5.1f}%")
    print(f"  {'zstd(shuffle)':<24} {size(shuffled):>9}  {100*size(shuffled)/len(raw):5.1f}%")
    print(f"  {'zstd(shuffle+bytedelta)':<24} {size(both):>9}  {100*size(both)/len(raw):5.1f}%")

    gain = size(raw) - size(both)
    if gain <= 0:
        print("  -> the filters do not pay here; a column transform would be complexity for nothing")
    else:
        print(f"  -> the filters save {gain} bytes ({100*gain/size(raw):.0f}% of the compressed size)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
