#!/usr/bin/env python3
"""Fail when the licence record drifts from the sources it describes.

Three things are checked, all against files that are actually on disk:

1. every file named in LICENSES/INDEX.json exists and still has the recorded
   SHA-256 — so a notice cannot be edited or deleted without the record moving;
2. every id in SOURCES has an entry in INDEX.json — so a new source cannot be
   added without saying something about its licence;
3. every SPDX identifier used by a row has its canonical text in LICENSES/.

Rows whose licence could not be established are *listed*, not failed: that is a
decision for a person. They are never treated as resolved.

    python3 tools/check-licences.py            # exit 1 on drift
    python3 tools/check-licences.py --strict   # also exit 1 on unresolved rows
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIC = ROOT / "LICENSES"
INDEX = LIC / "INDEX.json"
SOURCES = ROOT / "SOURCES"


# Tables in SOURCES that are not claims about a vendored source. A row there is
# prose about the manifest — "Pin provenance", "Licence corrections" — and its
# first cell is not a source id.
NOT_SOURCE_TABLES = {"Pin provenance", "Licence corrections", "Pending Additions",
                     "Submodule entries"}

SOURCE_ID = re.compile(r"^[A-Za-z0-9_.+-]+$")


def section_key(section: str) -> str:
    return re.sub(r"\s*\(.*\)\s*$", "", section).strip()


def source_ids() -> list[str]:
    """Ids from the source tables only.

    An earlier version read every table in the file, so adding a provenance table
    made the checker report ids like `xz` — backticks included — as sources with no
    licence entry. The id must be a bare token and the table must be a source
    table.
    """
    ids, seen, section = [], set(), ""
    for line in SOURCES.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if section_key(section) in NOT_SOURCE_TABLES:
            continue
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5 or cells[0] == "id" or set(cells[0]) <= set("-: "):
            continue
        if not SOURCE_ID.match(cells[0]):
            continue
        if cells[0] in seen:
            continue
        seen.add(cells[0])
        ids.append(cells[0])
    return ids


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--strict", action="store_true",
                    help="also fail when a row's licence is still unresolved")
    args = ap.parse_args()

    if not INDEX.is_file():
        print(f"FAIL  {INDEX.relative_to(ROOT)} is missing")
        return 1

    data = json.loads(INDEX.read_text(encoding="utf-8"))
    failures: list[str] = []

    # 1. recorded files must be present and unchanged
    for rec in data["sources"]:
        for ev in rec["evidence"]:
            f = LIC / ev["file"]
            if not f.is_file():
                failures.append(f"{rec['id']}: recorded notice {ev['file']} is missing")
                continue
            got = hashlib.sha256(f.read_bytes()).hexdigest()
            if got != ev["sha256"]:
                failures.append(f"{rec['id']}: {ev['file']} changed "
                                f"(recorded {ev['sha256'][:12]}, found {got[:12]})")

    # 2. every SOURCES id has a licence record
    recorded = {rec["id"] for rec in data["sources"]}
    for sid in source_ids():
        if sid not in recorded:
            failures.append(f"{sid}: in SOURCES but has no entry in LICENSES/INDEX.json")

    # 3. every SPDX id used has its canonical text
    for rec in data["sources"]:
        for spdx in rec["spdx"]:
            if not (LIC / f"{spdx}.txt").is_file():
                failures.append(f"{rec['id']}: canonical text {spdx}.txt is missing")

    unresolved = [rec for rec in data["sources"] if not rec["evidence"]]
    collected = len(data["sources"]) - len(unresolved)

    for f in failures:
        print(f"FAIL  {f}")
    if unresolved:
        print(f"\n{len(unresolved)} row(s) still unresolved — a decision, not a defect:")
        for rec in unresolved:
            print(f"  - {rec['id']:12s} {rec['status']} ({rec['url']})")

    print(f"\n{collected}/{len(data['sources'])} sources carry licence evidence; "
          f"{len(data['spdx_texts'])} canonical SPDX texts; {len(failures)} failure(s)")

    if failures:
        return 1
    if args.strict and unresolved:
        print("strict: unresolved rows are a failure")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
