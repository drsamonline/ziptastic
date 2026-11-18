#!/usr/bin/env python3
"""pareto.py — report the speed/size frontier, not one number.

Why this exists
---------------
`bench` prints one row per file at one setting, and the docs have quoted single
numbers ever since. A single number hides the choice: the same file at three
settings is three different products, and which one is "best" depends on whether
you are paying for bytes or for seconds.

The 2026 AIT Data Compression Challenge ranked 117 compressors on exactly this
shape and concluded that *no single compressor dominates across all criteria*
(audit/docs/compression-links-2026-11-18.md item 3). This script applies the same
rule to ziptastic's own settings: run every setting on every file, then keep only
the points nothing else beats on both axes.

A point is on the frontier when no other point is **both** smaller **and**
faster. Points on it are the only ones worth quoting.

Usage
-----
    scripts/pareto.py --bin ./ziptastic --corpus /path/to/dir
    scripts/pareto.py --bin ./ziptastic --corpus dir --settings -9,--png-fast

Exit codes: 0 all files measured, 1 a file failed, 2 usage.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import time

# The settings worth comparing: the fast form, the default, max effort without the
# CM backend, and the CM backend. Each is a different product, not a different
# quality setting of the same one.
DEFAULT_SETTINGS = ["--png-fast", "", "-9", "--max-ratio"]

ROW = re.compile(r"^(?P<file>\S.*?)\s+(?P<raw>\d+)\s+(?P<chce>\d+)\s+(?P<ratio>[\d.]+)%\s+(?P<mbs>[\d.]+)$")


def measure(binary: str, path: pathlib.Path, flags: str) -> dict | None:
    """Run `bench` at one setting and return the parsed row."""
    argv = [binary, "bench"]
    if flags:
        argv.extend(flags.split())
    argv.append(str(path))
    # Time the run here rather than reading bench's MB/s column: that column is
    # printed to one decimal, so anything slower than ~0.05 MB/s rounds to "0.0" and
    # a frontier built from it would silently drop the slow, high-ratio settings —
    # which are the ones a frontier exists to show.
    t0 = time.monotonic()
    try:
        out = subprocess.run(argv, capture_output=True, text=True, timeout=1800)
    except subprocess.TimeoutExpired:
        return None
    elapsed = time.monotonic() - t0
    if out.returncode != 0:
        return None
    for line in out.stdout.splitlines():
        m = ROW.match(line.strip())
        if m and not line.startswith("TOTAL") and not line.startswith("file"):
            raw, chce = int(m["raw"]), int(m["chce"])
            return {"setting": flags or "(default)", "raw": raw, "chce": chce,
                    "seconds": elapsed, "mbs": raw / elapsed / 1e6 if elapsed > 0 else 0.0}
    return None


def frontier(points: list[dict]) -> list[dict]:
    """The points no other point beats on both size and time."""
    kept = []
    for p in points:
        dominated = any(
            (q["chce"] <= p["chce"] and q["seconds"] <= p["seconds"])
            and (q["chce"] < p["chce"] or q["seconds"] < p["seconds"])
            for q in points
        )
        if not dominated:
            kept.append(p)
    return sorted(kept, key=lambda p: p["chce"])


def selftest() -> int:
    """Assert the frontier rule on points whose answer is known by inspection.

    Kept in the script rather than in the C++ suite because it is pure Python and
    needs no build: the rule is easy to get subtly wrong (a point that ties on one
    axis must stay, one that loses on both must go) and this costs a second to run.
    """
    cases = [
        # same bytes, faster: the slow one is dominated
        ([{"setting": "a", "chce": 100, "seconds": 5.0},
          {"setting": "b", "chce": 100, "seconds": 2.0}], {"b"}),
        # smaller but slower: both stay
        ([{"setting": "a", "chce": 100, "seconds": 1.0},
          {"setting": "b", "chce": 50, "seconds": 9.0}], {"a", "b"}),
        # a trade-off: each wins on one axis, so all three stay
        ([{"setting": "a", "chce": 100, "seconds": 1.0},
          {"setting": "b", "chce": 50, "seconds": 9.0},
          {"setting": "c", "chce": 60, "seconds": 8.0}], {"a", "b", "c"}),
        # d is larger AND slower than c, so d goes
        ([{"setting": "a", "chce": 100, "seconds": 1.0},
          {"setting": "c", "chce": 60, "seconds": 8.0},
          {"setting": "d", "chce": 70, "seconds": 10.0}], {"a", "c"}),
        # one point: it is the frontier
        ([{"setting": "a", "chce": 100, "seconds": 1.0}], {"a"}),
    ]
    for i, (points, expected) in enumerate(cases, 1):
        got = {p["setting"] for p in frontier(points)}
        if got != expected:
            print(f"selftest case {i} FAILED: expected {sorted(expected)}, got {sorted(got)}",
                  file=sys.stderr)
            return 1
    print(f"selftest: {len(cases)} cases pass")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", help="the ziptastic binary")
    ap.add_argument("--corpus", help="directory of files to measure")
    ap.add_argument("--settings", default=",".join(DEFAULT_SETTINGS),
                    help="comma-separated flag sets; empty string means the default")
    ap.add_argument("--selftest", action="store_true",
                    help="check the frontier rule on known points and exit")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.bin or not args.corpus:
        ap.error("--bin and --corpus are required unless --selftest is given")

    corpus = pathlib.Path(args.corpus)
    if not corpus.is_dir():
        print(f"pareto: not a directory: {corpus}", file=sys.stderr)
        return 2
    settings = args.settings.split(",")
    files = sorted(p for p in corpus.iterdir() if p.is_file())
    if not files:
        print(f"pareto: no files in {corpus}", file=sys.stderr)
        return 2

    failures = 0
    print(f"{'file':<20} {'setting':<12} {'bytes':>10} {'seconds':>9} {'frontier':>9}")
    print("-" * 64)
    for path in files:
        points = []
        for flags in settings:
            row = measure(args.bin, path, flags)
            if row is None:
                failures += 1
                print(f"{path.name:<20} {flags or '(default)':<12} {'-':>10} {'-':>9} {'FAILED':>9}")
                continue
            points.append(row)
        if not points:
            continue
        best = frontier(points)
        names = {id(p) for p in best}
        for p in sorted(points, key=lambda p: p["chce"]):
            mark = "*" if id(p) in names else ""
            print(f"{path.name:<20} {p['setting']:<12} {p['chce']:>10} {p['seconds']:>9.2f} {mark:>9}")
        if len(best) == 1:
            print(f"{'':<20} {'':<12} one point dominates the rest — the choice is made for you")
        print()

    if failures:
        print(f"{failures} measurement(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
