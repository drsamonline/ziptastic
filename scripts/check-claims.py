#!/usr/bin/env python3
"""check-claims.py — every number in the README table must exist in FINDINGS.md.

`docs/FINDINGS.md` calls itself "the single authoritative record". The README
repeats six of its rows for the headline claim, and nothing checked that the two
still said the same thing — a hand-copied table is exactly how a published number
drifts from the measurement behind it.

    python3 scripts/check-claims.py
    python3 scripts/check-claims.py --verbose

Exit 0 when every README row's (ziptastic, xz) pair appears in FINDINGS.md.
Rows are matched by their numbers, not their labels: the README names a case
"Mixed game-asset tree, 6.3 MB, as `.tar`" where FINDINGS names it "`tree.tar`",
and requiring the labels to agree would fail on wording rather than on a number.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NUM = re.compile(r"\d[\d,]*")


def numbers(cell: str) -> list[int]:
    return [int(m.replace(",", "")) for m in NUM.findall(cell)]


def table_rows(text: str) -> list[list[str]]:
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) >= 3 and not set("".join(cells)) <= set("-: "):
            rows.append(cells)
    return rows


def main() -> int:
    verbose = "--verbose" in sys.argv
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    findings = (ROOT / "docs" / "FINDINGS.md").read_text(encoding="utf-8")

    # A FINDINGS row may carry more than two measurements (ziptastic, gzip, xz),
    # so every later numeric cell is a candidate the README may be quoting.
    known: set[tuple[int, int]] = set()
    for cells in table_rows(findings):
        first = numbers(cells[1]) if len(cells) > 2 else []
        if not first:
            continue
        for cell in cells[2:]:
            for other in numbers(cell):
                known.add((first[0], other))

    checked = 0
    missing = []
    for cells in table_rows(readme):
        a, b = numbers(cells[1]), numbers(cells[2])
        # Byte counts, not the `-9` / `-9e` flag in the header row.
        if not a or not b or a[0] < 100 or b[0] < 100:
            continue
        pair = (a[0], b[0])
        checked += 1
        if pair not in known:
            missing.append((cells[0], pair))
        elif verbose:
            print(f"  ok  {cells[0]}: {pair[0]:,} / {pair[1]:,}")

    print(f"README claim rows checked: {checked}")
    if missing:
        for label, pair in missing:
            print(f"  NOT IN FINDINGS: {label} -> ziptastic {pair[0]:,}, xz {pair[1]:,}")
        print(f"\n{len(missing)} row(s) of the README table are not in FINDINGS.md.")
        print("Either the number changed in one place and not the other, or the row")
        print("was never measured there. FINDINGS.md is the record; the README quotes it.")
        return 1
    print("every README number appears in FINDINGS.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
