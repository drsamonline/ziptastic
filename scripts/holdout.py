#!/usr/bin/env python3
"""holdout.py — the files a change was not tuned on.

Why this exists
---------------
Every single-fixture mistake this project has made had the same shape: a number
measured on files the author chose. The 64 KiB sample, the formula PNG, the gzip
XML, the LZ4 synthetic frame, the 99% column-filter saving — five in one day.

The fix is not discipline, it is a split. This script measures a **holdout** set:
files that no transform was designed against, listed in a manifest with their
hashes, so a release can be checked against the set instead of against the files
it was tuned on.

The rule for what belongs here (deliberately mechanical, so it is not taste):
files that are **in the repository and are not in the development corpus** — the
project's own documentation, its built binary, an archive of the vendor tree.
Nothing here was chosen because a transform does well on it.

Usage
-----
    scripts/holdout.py --bin ./ziptastic --dir holdout --record
    scripts/holdout.py --bin ./ziptastic --dir holdout --check

`--record` writes holdout/MANIFEST.tsv (path, sha256, bytes, archive bytes).
`--check` verifies every hash still matches, re-measures, and reports any file
whose archive grew against the recorded baseline — a regression, in bytes.

Exit: 0 clean, 1 regression or hash drift, 2 usage.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys

MANIFEST = "MANIFEST.tsv"


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def measure(binary: str, path: pathlib.Path, work: pathlib.Path) -> int | None:
    out = work / (path.name + ".hzpkg")
    r = subprocess.run([binary, "-9", "-f", "-o", str(out), str(path)],
                       capture_output=True, text=True, timeout=3600)
    if r.returncode != 0 or not out.exists():
        return None
    n = out.stat().st_size
    out.unlink()
    return n


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--dir", default="holdout")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if not (args.record or args.check):
        ap.error("give --record or --check")

    root = pathlib.Path(args.dir)
    if not root.is_dir():
        print(f"holdout: no such directory: {root}", file=sys.stderr)
        return 2
    files = sorted(p for p in root.rglob("*") if p.is_file() and p.name != MANIFEST)
    if not files:
        print(f"holdout: nothing in {root}", file=sys.stderr)
        return 2
    manifest = root / MANIFEST
    work = pathlib.Path("/tmp")

    if args.record:
        rows = ["path\tsha256\tbytes\tarchive"]
        for p in files:
            size = measure(args.bin, p, work)
            if size is None:
                print(f"holdout: failed to compress {p}", file=sys.stderr)
                return 1
            rows.append(f"{p.relative_to(root)}\t{sha256(p)}\t{p.stat().st_size}\t{size}")
            print(f"  recorded {p.name}: {p.stat().st_size} -> {size}")
        manifest.write_text("\n".join(rows) + "\n")
        total_in = sum(int(r.split("\t")[2]) for r in rows[1:])
        total_out = sum(int(r.split("\t")[3]) for r in rows[1:])
        print(f"\n{len(files)} files, {total_in} -> {total_out} bytes "
              f"({100*total_out/total_in:.1f}%)")
        print(f"manifest: {manifest}")
        return 0

    if not manifest.exists():
        print(f"holdout: no manifest at {manifest} — run --record first", file=sys.stderr)
        return 2
    rows = [line.split("\t") for line in manifest.read_text().splitlines()[1:] if line]
    drift = 0
    regressed = 0
    print(f"{'file':<28} {'bytes':>10} {'archive':>10} {'was':>10} {'delta':>8}")
    print("-" * 70)
    for rel, want_hash, want_bytes, want_archive in rows:
        p = root / rel
        if not p.exists():
            print(f"{rel:<28} MISSING")
            drift += 1
            continue
        if sha256(p) != want_hash:
            print(f"{rel:<28} HASH DRIFT")
            drift += 1
            continue
        got = measure(args.bin, p, work)
        if got is None:
            print(f"{rel:<28} FAILED")
            regressed += 1
            continue
        delta = got - int(want_archive)
        mark = "" if delta <= 0 else "  REGRESSED"
        if delta > 0:
            regressed += 1
        print(f"{rel:<28} {int(want_bytes):>10} {got:>10} {int(want_archive):>10} {delta:>+8}{mark}")

    if drift or regressed:
        print(f"\n{drift} hash drift, {regressed} regression(s)", file=sys.stderr)
        return 1
    print("\nholdout clean: hashes match and nothing grew")
    return 0


if __name__ == "__main__":
    sys.exit(main())
