#!/usr/bin/env python3
"""Build HCS/LICENSES from evidence, not from memory.

- Canonical licence texts are fetched from spdx/license-list-data (the same text
  GitHub and SPDX publish), never typed from recollection.
- Per-source notices are copied verbatim from the vendored tree under old/ where
  one exists; otherwise fetched from the upstream repository at HEAD.
- Everything that could not be established is recorded as unresolved, with the
  reason. Nothing is guessed.
"""
import concurrent.futures as cf
import hashlib
import json
import re
import shutil
import urllib.error
import urllib.request
from pathlib import Path

HCS = Path(__file__).resolve().parent.parent
OLD = HCS / "old"
OUT = HCS / "LICENSES"
SPDX_RAW = "https://raw.githubusercontent.com/spdx/license-list-data/main/text/{}.txt"

LIC_NAMES = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)([-_.].*)?$", re.I)
UPSTREAM_NAMES = ["LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING", "COPYING.txt",
                  "LICENCE", "LICENSE-MIT", "LICENSE.rst", "COPYRIGHT"]

# licence-cell token -> SPDX id (only where the mapping is unambiguous)
TOKENS = {
    "0BSD": "0BSD", "LGPL-2.1": "LGPL-2.1-only", "LGPL-3.0": "LGPL-3.0-only",
    "GPL-2.0": "GPL-2.0-only", "GPL-3.0": "GPL-3.0-only",
    "GPL-2.0-or-later": "GPL-2.0-or-later", "GPL-3.0-or-later": "GPL-3.0-or-later",
    "SSPL-1.0": "SSPL-1.0",
    "Apache-2.0": "Apache-2.0", "MIT": "MIT", "MIT-0": "MIT-0",
    "BSD-2-Clause": "BSD-2-Clause", "BSD-3-Clause": "BSD-3-Clause",
    "ISC": "ISC", "zlib": "Zlib", "MPL-2.0": "MPL-2.0",
    "CC0-1.0": "CC0-1.0", "Unlicense": "Unlicense",
}
NON_SPDX = {"public-domain", "all-rights-reserved", "commercial", "verify", "cc",
            "mixed", "fill-in"}


def norm_cell(cell):
    """Split a SOURCES licence cell into SPDX ids plus flags we cannot map."""
    raw = cell.strip()
    low = raw.lower()
    if low in ("verify", "mixed", "cc") or raw == "":
        return [], [low or "blank"]
    parts = re.split(r"[+,/]| and | or ", raw)
    ids, flags = [], []
    for p in parts:
        p = p.strip()
        if not p:
            continue
        if p in TOKENS:
            ids.append(TOKENS[p])
        elif p.lower() in ("pd", "public-domain", "public domain"):
            flags.append("public-domain")
        elif p.lower() in ("commercial",):
            flags.append("commercial")
        else:
            flags.append(p.lower())
    return sorted(set(ids)), sorted(set(flags))


def fetch(url, timeout=25):
    req = urllib.request.Request(url, headers={"User-Agent": "hcs-licence-collector"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            if r.status != 200:
                return None
            return r.read()
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, OSError):
        return None


def sha(b):
    return hashlib.sha256(b).hexdigest()


# Tables in SOURCES that are prose about the manifest, not claims about a vendored
# source. Same rule as check-licences.py: without it, a row like
# `| \`freearc-c\` | \`verify\` | \`GPL-2.0\` | … |` in Licence corrections is read
# as a source named with its backticks still attached. Both scripts carry this
# filter because they are run independently; if a third appears, move it into one.
NOT_SOURCE_TABLES = {"Pin provenance", "Licence corrections", "Pending Additions",
                     "Submodule entries"}
SOURCE_ID = re.compile(r"^[A-Za-z0-9_.+-]+$")


def section_key(section: str) -> str:
    return re.sub(r"\s*\(.*\)\s*$", "", section).strip()


def parse_sources():
    rows, seen, section = [], set(), ""
    for line in (HCS / "SOURCES").read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
            continue
        if section_key(section) in NOT_SOURCE_TABLES:
            continue
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5:
            continue
        if cells[0] in ("id",) or set(cells[0]) <= set("-: "):
            continue
        if not SOURCE_ID.match(cells[0]):
            continue
        if cells[0] in seen:
            continue
        seen.add(cells[0])
        rows.append({"id": cells[0], "url": cells[1], "pin": cells[2],
                     "license": cells[3], "integration": cells[4]})
    return rows


def unique_target(name):
    """Avoid two notices from one tree overwriting each other."""
    t = OUT / name
    if not t.exists():
        return name
    stem, dot, ext = name.rpartition(".")
    i = 2
    while True:
        cand = f"{stem}-{i}.{ext}" if dot else f"{name}-{i}"
        if not (OUT / cand).exists():
            return cand
        i += 1


def readme_licence_section(d):
    """The licence section of the source's own README, if it states one."""
    for nm in ("README.md", "README", "README.txt", "readme.md"):
        f = Path(d) / nm
        if not f.is_file():
            continue
        txt = f.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"^#{1,3}\s*(licen[cs]e|licensing)\s*$", txt, re.I | re.M)
        if not m:
            continue
        start = m.start()
        rest = txt[m.end():]
        nxt = re.search(r"^#{1,3}\s+\S", rest, re.M)
        block = txt[start: m.end() + (nxt.start() if nxt else len(rest))]
        if HEADER_HINT.search(block):
            return f, block.strip() + "\n"
    return None


def tree_licence_files(d):
    out = []
    for dirpath, dirnames, filenames in Path(d).walk():
        depth = len(Path(dirpath).relative_to(d).parts)
        if depth >= 3:
            dirnames[:] = []
        for f in sorted(filenames):
            if LIC_NAMES.match(f):
                p = Path(dirpath) / f
                if p.stat().st_size < 200_000:
                    out.append(p)
    return out


CODE_START = re.compile(r"^(#include|#define|#ifndef|#pragma|#if|using |int |char |void |static |class |struct |namespace |template|def |import |fn |let |pub |const |unsigned |long |short |float |double |bool |extern |typedef |enum )")
HEADER_HINT = re.compile(r"(copyright|licen[cs]e|public domain|all rights reserved)", re.I)

# Files whose header is the project's own licence statement, where the file name
# does not carry the source id. Chosen by hand, one per source, and named here so
# the choice is reviewable rather than hidden in a heuristic.
# Licences the vault cannot establish from a text because the vendored copy has
# none, recorded on the owner's statement instead. Kept separate from collected
# evidence: a statement is a fact about provenance, not a licence text, and the
# record says which one it is.
OWNER_STATED = {
    "LPAQ9": (
        "GPL-2.0",
        "The vendored copy is `lpaq9m.zpaq`, a binary archive containing no source\n"
        "and no licence notice, and `SOURCES` records only a zip URL this vault does\n"
        "not mirror. The licence is recorded on the maintainer's statement of\n"
        "15 November 2026 that LPAQ carries the same licence as paq8, which\n"
        "`SOURCES` records as GPL-2.0.\n\n"
        "This file is a record of where the claim comes from, not the licence text.\n"
        "The text is `LICENSES/GPL-2.0-only.txt`, and it applies if the statement is\n"
        "right. If the upstream archive is ever fetched and its readme read, replace\n"
        "this file with what it says.\n"
    ),
}

HEADER_OVERRIDES = {
    "lzma":    "old/lzma/C/LzmaDec.c",
    "FPAQF2":  "old/FPAQF2/fpaq0f2.cpp",
}


def find_header_notice(root, src_id, limit=400):
    """Licence header from a file that belongs to the source itself.

    Only a file whose name carries the source id is considered: a vendored tree
    usually also contains bundled third-party code, and the alphabetically first
    file in paq8px is zlib's adler32.c, not paq8px.
    """
    exts = (".c", ".h", ".cpp", ".hpp", ".cc", ".py", ".rs", ".go", ".js")
    want = re.sub(r"[^a-z0-9]", "", src_id.lower())
    override = HEADER_OVERRIDES.get(src_id)
    candidates = []
    if override:
        op = Path(HCS) / override
        if op.is_file():
            candidates.append(op)
    for p in sorted(Path(root).rglob("*")):
        if p.is_file() and p.suffix.lower() in exts:
            if want and want in re.sub(r"[^a-z0-9]", "", p.name.lower()):
                candidates.append(p)
    for p in candidates:
        if p.suffix.lower() not in exts:
            continue
        try:
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()[:120]
        except OSError:
            continue

        # 1. a /* ... */ block anywhere in the head that states a licence
        opens = [i for i, ln in enumerate(lines) if "/*" in ln]
        closes = [i for i, ln in enumerate(lines) if "*/" in ln]
        for o in opens:
            c = next((x for x in closes if x >= o), None)
            if c is None:
                continue
            block = "\n".join(lines[o:c + 1]).strip()
            if HEADER_HINT.search(block) and len(block) > 60:
                return p, block + "\n"

        # 2. a // or # comment run in the head that states a licence
        keep = []
        for ln in lines:
            st = ln.strip()
            if CODE_START.match(st) or st.endswith(";"):
                if keep and HEADER_HINT.search("\n".join(keep)):
                    return p, "\n".join(keep).strip() + "\n"
                keep = []
                continue
            keep.append(ln)
        if keep and HEADER_HINT.search("\n".join(keep)):
            return p, "\n".join(keep).strip() + "\n"
    return None


def gh_parts(url):
    m = re.match(r"https?://github\.com/([^/]+)/([^/#?]+)", url.strip())
    return (m.group(1), m.group(2).removesuffix(".git")) if m else None


def main():
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "sources").mkdir(parents=True)

    rows = parse_sources()
    dirs = sorted(d.name for d in OLD.iterdir() if d.is_dir())

    def match_dir(sid):
        s = sid.lower()
        for d in dirs:
            dl = d.lower()
            if dl == s or dl.startswith(s + "-") or dl.startswith(s + "_"):
                return d
        return None

    # 1. which SPDX texts do we need
    needed = set()
    for r in rows:
        ids, _ = norm_cell(r["license"])
        needed.update(ids)

    print(f"rows: {len(rows)}   SPDX texts needed: {len(needed)}")
    with cf.ThreadPoolExecutor(max_workers=8) as ex:
        got = list(ex.map(lambda i: (i, fetch(SPDX_RAW.format(i))), sorted(needed)))
    spdx_index = {}
    for sid, body in got:
        if body and len(body) > 400:
            (OUT / f"{sid}.txt").write_bytes(body)
            spdx_index[sid] = {"file": f"{sid}.txt", "bytes": len(body), "sha256": sha(body),
                               "source": SPDX_RAW.format(sid)}
            print(f"   SPDX {sid:16s} {len(body):6d} B")
        else:
            spdx_index[sid] = {"file": None, "error": "not fetched from SPDX list"}
            print(f"   SPDX {sid:16s} FETCH FAILED")

    # 2. per-source evidence
    records = []
    for r in rows:
        ids, flags = norm_cell(r["license"])
        d = match_dir(r["id"])
        files, status, evidence = [], "unresolved", []

        if d:
            for p in tree_licence_files(OLD / d):
                b = p.read_bytes()
                name = unique_target(f"sources/{r['id']}-{p.name}")
                (OUT / name).write_bytes(b)
                evidence.append({"file": name, "origin": str(p.relative_to(HCS)),
                                 "bytes": len(b), "sha256": sha(b)})
            if evidence:
                status = "collected from vendored tree"

        # fallback: a licence recorded on the owner's statement
        if not evidence and r["id"] in OWNER_STATED:
            licence, note = OWNER_STATED[r["id"]]
            name = f"sources/{r['id']}-OWNER-STATEMENT.txt"
            (OUT / name).write_text(note, encoding="utf-8")
            evidence.append({"file": name,
                             "origin": "maintainer statement, 15 Nov 2026",
                             "bytes": len(note), "sha256": sha(note.encode()),
                             "note": f"no licence text in the vault; recorded as {licence}"})
            status = "recorded from the maintainer's statement; no text in the vault"

        # fallback: a licence section in the source's own README
        if not evidence and d:
            rl = readme_licence_section(OLD / d)
            if rl:
                f, block = rl
                name = f"sources/{r['id']}-README-licence.txt"
                (OUT / name).write_text(block, encoding="utf-8")
                evidence.append({"file": name, "origin": str(f.relative_to(HCS)),
                                 "bytes": len(block), "sha256": sha(block.encode()),
                                 "note": "licence section quoted from the source's own README"})
                status = "collected from the source's README licence section"

        # fallback: a copyright/licence header inside the vendored sources
        if not evidence and d:
            hdr = find_header_notice(OLD / d, r['id'])
            if hdr:
                p, block = hdr
                name = f"sources/{r['id']}-HEADER.txt"
                (OUT / name).write_text(block, encoding="utf-8")
                evidence.append({"file": name, "origin": str(p.relative_to(HCS)),
                                 "bytes": len(block), "sha256": sha(block.encode()),
                                 "note": "copyright/licence header from a vendored source file"})
                status = "collected from a source-file header"

        if not evidence:
            parts = gh_parts(r["url"])
            if parts:
                owner, repo = parts
                for nm in UPSTREAM_NAMES:
                    url = f"https://raw.githubusercontent.com/{owner}/{repo}/HEAD/{nm}"
                    b = fetch(url, timeout=20)
                    if b and len(b) > 100:
                        name = f"sources/{r['id']}-{nm}"
                        (OUT / name).write_bytes(b)
                        evidence.append({"file": name, "origin": url,
                                         "bytes": len(b), "sha256": sha(b)})
                        status = "collected from upstream HEAD"
                        break
                if not evidence:
                    status = "upstream reachable but no licence file at HEAD"
            elif r["url"].startswith("(fill"):
                status = "no URL and no vendored licence file"
            else:
                status = "no vendored licence file; not a GitHub URL, not fetched"

        if d and not any((OLD / d).rglob("*")):
            status = "vendored directory is empty; no evidence to collect"

        records.append({"id": r["id"], "url": r["url"], "declared": r["license"],
                        "spdx": ids, "flags": flags, "vendored_dir": d,
                        "status": status, "evidence": evidence})
        print(f"   {r['id']:20s} {r['license']:20s} {status}")

    (OUT / "INDEX.json").write_text(json.dumps(
        {"generated_from": "HCS/SOURCES and HCS/old (vendored trees)",
         "spdx_texts": spdx_index, "sources": records}, indent=1), encoding="utf-8")

    # 3. README
    resolved = [r for r in records if r["evidence"]]
    lines = [
        "# HCS third-party licence texts",
        "",
        "Every file here was collected from a real location; nothing was written from",
        "recollection. `INDEX.json` carries the provenance (origin path or URL) and the",
        "SHA-256 of every file.",
        "",
        "Two kinds of file:",
        "",
        "- `<SPDX-ID>.txt` — the canonical licence text, fetched from the SPDX licence",
        "  list data (`spdx/license-list-data`), the same text GitHub and SPDX publish.",
        "- `sources/<id>-<name>` — the notice exactly as it ships with that source,",
        "  copied verbatim from the vendored tree under `old/` where one exists,",
        "  otherwise fetched from the upstream repository at HEAD.",
        "",
        f"Collected: {len(resolved)} of {len(records)} sources. Unresolved rows are",
        "listed below with the reason; they are not silently treated as licensed.",
        "",
        "## Sources",
        "",
        "| id | SOURCES says | SPDX text | Notice file | Status |",
        "|---|---|---|---|---|",
    ]
    for r in records:
        spdx_cell = ", ".join(f"[{s}]({s}.txt)" for s in r["spdx"]) or "—"
        notice = ", ".join(f"`{e['file'].split('/')[-1]}`" for e in r["evidence"]) or "—"
        lines.append(f"| `{r['id']}` | {r['declared']} | {spdx_cell} | {notice} | {r['status']} |")
    lines += ["", "## What is still open", ""]
    for r in records:
        if not r["evidence"]:
            lines.append(f"- `{r['id']}` — {r['status']} ({r['url']}).")
    (OUT / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"\ncollected {len(resolved)}/{len(records)}")
    print(f"files: {len(list(OUT.rglob('*.*')))}")


if __name__ == "__main__":
    main()
