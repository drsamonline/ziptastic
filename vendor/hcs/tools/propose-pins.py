#!/usr/bin/env python3
"""Propose a pin for a `copy` row from the version the tree itself states.

    python3 tools/propose-pins.py        # writes pin-proposals.json, changes nothing

The last run pinned five of twenty-nine: xz, brotli, zpaq, libbsc, libdeflate.
Two more state a version no upstream tag matches (zstd 1.6.0, lz4 1.10.1 — the
trees are ahead of the last release), and the rest state none this vault can read.
Re-run it after a vendor bump; anything newly pinnable will show up.

Per source, one explicit place to look. A table rather than a heuristic: a
version scraped from the wrong file is worse than no pin at all, and every entry
here can be checked by opening the file it names.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

HCS = Path(__file__).resolve().parent.parent
OLD = HCS / "old"

# id -> (file relative to old/<dir>, how to read it)
STRATEGIES = {
    "libbsc":      ("VERSION", "string"),
    "zstd":        ("lib/zstd.h", "triple:ZSTD_VERSION"),
    "lz4":         ("lib/lz4.h", "triple:LZ4_VERSION"),
    "brotli":      ("c/common/version.h", "triple:BROTLI_VERSION"),
    "libdeflate":  ("libdeflate.h", 'quoted:LIBDEFLATE_VERSION_STRING'),
    "zpaq":        ("zpaq.cpp", 'quoted:ZPAQ_VERSION'),
    "xz":          ("src/liblzma/api/lzma/version.h", "triple:LZMA_VERSION"),
}

ROW = re.compile(r"^\|\s*([A-Za-z0-9_.+-]+)\s*\|\s*(\S+)\s*\|\s*(\S+)\s*\|\s*(\S.*?)\s*\|\s*(\S+)\s*\|\s*$")


def read_manifest():
    rows, seen = [], set()
    for line in (HCS / "SOURCES").read_text().splitlines():
        m = ROW.match(line.rstrip("\n"))
        if not m:
            continue
        sid, url, pin, lic, integ = m.groups()
        if sid == "id" or set(sid) == {"-"} or sid in seen:
            continue
        seen.add(sid)
        rows.append({"id": sid, "url": url, "pin": pin})
    return rows


def resolve_dir(sid, dirs):
    s = sid.lower()
    best = None
    for d in dirs:
        dl = d.lower()
        if dl == s or dl.startswith(s + "-") or dl.startswith(s + "_"):
            if best is None or len(d) > len(best):
                best = d
    return best


def extract(path: Path, how: str):
    if not path.is_file():
        return None
    txt = path.read_text(encoding="utf-8", errors="replace")
    kind, _, key = how.partition(":")
    if kind == "string":
        m = re.search(r"([0-9]+\.[0-9]+(?:\.[0-9]+)?)", txt)
        return m.group(1) if m else None
    if kind == "quoted":
        m = re.search(rf'{re.escape(key)}\s+"([0-9][0-9.]*)"', txt)
        return m.group(1) if m else None
    if kind == "triple":
        parts = []
        for field in ("MAJOR", "MINOR", "RELEASE", "PATCH"):
            m = re.search(rf"#\s*define\s+{re.escape(key)}_{field}\s+(\d+)", txt)
            if m:
                parts.append(m.group(1))
        return ".".join(parts) if len(parts) >= 2 else None
    return None


def gh_parts(url):
    m = re.match(r"https?://github\.com/([^/]+)/([^/]+)", url.strip())
    return (m.group(1), m.group(2).removesuffix(".git")) if m else None


def tags_for(owner, repo):
    try:
        out = subprocess.run(
            ["gh", "api", f"repos/{owner}/{repo}/tags?per_page=100",
             "--jq", '.[] | "\\(.name)\\t\\(.commit.sha)"'],
            capture_output=True, text=True, timeout=90)
    except (subprocess.SubprocessError, OSError):
        return None
    if out.returncode != 0:
        return None
    return [tuple(l.split("\t", 1)) for l in out.stdout.splitlines() if "\t" in l]


def main():
    rows = [r for r in read_manifest() if r["pin"] == "copy"]
    dirs = sorted(d.name for d in OLD.iterdir() if d.is_dir())
    out = []

    for r in rows:
        sid = r["id"]
        if sid not in STRATEGIES:
            out.append({**r, "verdict": "no version file identified for this source"})
            continue
        rel, how = STRATEGIES[sid]
        d_name = resolve_dir(sid, dirs)
        if not d_name:
            out.append({**r, "verdict": "no directory"}); continue
        path = OLD / d_name / rel
        version = extract(path, how)
        if not version:
            out.append({**r, "verdict": f"version not readable from {rel}"}); continue
        parts = gh_parts(r["url"])
        if not parts:
            out.append({**r, "version": version, "verdict": "not a GitHub url"}); continue
        owner, repo = parts
        tags = tags_for(owner, repo)
        if tags is None:
            out.append({**r, "version": version,
                        "verdict": "version found, upstream tags unreadable"}); continue
        exact = [(n, s) for (n, s) in tags if n.lstrip("vV") == version]
        if len(exact) == 1:
            out.append({**r, "version": version, "tag": exact[0][0], "sha": exact[0][1],
                        "evidence": f"old/{d_name}/{rel}", "verdict": "proposed"})
        elif len(exact) > 1:
            out.append({**r, "version": version,
                        "verdict": f"ambiguous tags: {[e[0] for e in exact]}"})
        else:
            near = [n for (n, _) in tags if version in n][:4]
            out.append({**r, "version": version,
                        "verdict": f"no tag equal to {version}" + (f"; near: {near}" if near else "")})

    for x in out:
        mark = "PIN " if x["verdict"] == "proposed" else "    "
        print(f"{mark}{x['id']:<13} {x.get('version','-'):<9} "
              f"{x.get('tag', x['verdict'])[:52]:<52} {x.get('evidence','')}")

    n = sum(1 for x in out if x["verdict"] == "proposed")
    print(f"\n{n} of {len(out)} copy rows pinned from the tree's own version")
    Path("pin-proposals.json").write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    sys.exit(main())
