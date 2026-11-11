#!/usr/bin/env python3
"""check-sources.py — does SOURCES still describe `old/`?

`scripts/check-vendor.sh` proves every directory under `old/` has content. It
never reads `SOURCES`, so the manifest — the file the README calls the
machine-readable list every consumer reads — can say anything at all:

  * an id that resolves to no directory (the vault vendors `xz-master`, the
    manifest says `xz`);
  * a directory with no row, i.e. a vendored source with no recorded upstream,
    licence or integration;
  * the same id twice in the enforced tables.

None of that fails a build today, and none of it is visible unless somebody
reads both sides by hand.

  python3 tools/check-sources.py            # exit 1 if anything is wrong
  python3 tools/check-sources.py --advisory # report, always exit 0

Sections that are not claims about the tree — `Pending Additions`,
`Submodule entries`, `Licence corrections` — are parsed but not enforced.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(ROOT, "old")
SOURCES = os.path.join(ROOT, "SOURCES")

ROW = re.compile(r"^\|\s*([A-Za-z0-9_.+-]+)\s*\|\s*(\S+)\s*\|\s*(\S+)\s*\|\s*(\S.*?)\s*\|\s*(\S+)\s*\|\s*$")
NOT_ENFORCED = {"Pending Additions", "Submodule entries", "Licence corrections"}


def read_manifest():
    """[(section, id, url)] for every table row, in file order."""
    rows, section = [], ""
    with open(SOURCES, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("## "):
                section = line[3:].strip()
                continue
            m = ROW.match(line.rstrip("\n"))
            if m and m.group(1) not in ("id",) and set(m.group(1)) != {"-"}:
                rows.append((section, m.group(1), m.group(2)))
    return rows


def resolve_all(rows, dirs):
    """Map every SOURCES id to a directory, and say which directories are left.

    An id resolves exactly, or by the -master/-dev/-main suffix the vault
    actually uses. When two ids could claim the same directory (`preflate` and
    `preflate-rs` against `preflate-master` and `preflate-rs-main`), the longest
    id wins — it is the more specific claim.
    """
    claimed, resolved = {}, {}
    for _section, ident, _url in sorted(rows, key=lambda r: -len(r[1])):
        if ident in dirs and ident not in claimed:
            claimed[ident] = ident
            resolved[ident] = ident
    for _section, ident, _url in sorted(rows, key=lambda r: -len(r[1])):
        if ident in resolved:
            continue
        for d in dirs:
            if d.startswith(ident + "-") and d not in claimed:
                claimed[d] = ident
                resolved[ident] = d
                break
    return resolved, [d for d in dirs if d not in claimed]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--advisory", action="store_true",
                    help="report problems but exit 0 (for a tree that is not repaired yet)")
    args = ap.parse_args()

    if not os.path.isdir(VENDOR):
        print("FAIL: no old/ directory — wrong working directory?", file=sys.stderr)
        return 1
    dirs = sorted(d for d in os.listdir(VENDOR) if os.path.isdir(os.path.join(VENDOR, d)))
    rows = read_manifest()

    enforced = [(s, i, u) for (s, i, u) in rows if s not in NOT_ENFORCED]
    problems = []

    resolved, unrecorded = resolve_all(rows, dirs)

    seen = {}
    for section, ident, _url in enforced:
        key = (section, ident)
        if key in seen:
            problems.append(f"duplicate id {ident!r} in section {section!r}")
        seen[key] = True
        if ident not in resolved:
            problems.append(f"SOURCES lists {ident!r} but old/ has no such directory "
                            f"(no exact match and no {ident}-* match)")

    for d in unrecorded:
        problems.append(f"old/{d} is vendored but has no SOURCES row "
                        f"(no upstream, licence or integration recorded)")

    print(f"SOURCES rows: {len(rows)} ({len(enforced)} enforced), old/ directories: {len(dirs)}")
    if not problems:
        print("sources OK: every directory is recorded and every recorded id resolves")
        return 0
    for p in problems:
        print("  " + p)
    print(f"\n{len(problems)} problem(s).")
    if args.advisory:
        print("advisory: not failing the build")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
