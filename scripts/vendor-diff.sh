#!/usr/bin/env bash
# vendor-diff.sh — what would syncing vendor/hcs to HCS main actually change?
#
# `check-vendor-current.sh` answers "is the vendored copy current?" with one hash of
# SOURCES and, since issue #25, no remedy. This answers the question that has to come
# next: what does a sync consist of? It is the plan, not the sync.
#
# Why a plan and not a sync: `vendor/hcs` here is a MATERIALISED mirror — the 24
# submodules HCS carries as gitlinks are real files in this tree, and 247 files exist
# here that HCS does not have. A tree-level merge in either direction therefore
# imports build artifacts and deletes the sources the build compiles (measured: 543
# files, -91,112 lines). Doing it correctly means copying HCS's tracked files,
# re-materialising 24 submodules at their pinned SHAs, and preserving the local
# extras — three operations, each with its own way to go wrong, which is why they are
# counted before anything is touched.
#
#   ./scripts/vendor-diff.sh              # classify, print, exit 1 if stale
#   ./scripts/vendor-diff.sh --apply      # refuses: not implemented (see below)
#   ./scripts/vendor-diff.sh --hcs <dir>  # use a local HCS checkout instead of cloning
#
# Exit 0 = nothing to do, 1 = stale (or --apply), 2 = could not run.
set -u

HCS_URL="${HCS_URL:-https://github.com/drsamonline/HCS.git}"
LOCAL="vendor/hcs"
MODE=report
HCS_DIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) MODE=apply ;;
    --hcs) HCS_DIR="${2:?--hcs needs a path}"; shift ;;
    -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "vendor-diff: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$LOCAL" ] || { echo "vendor-diff: no $LOCAL — run this from the repository root" >&2; exit 2; }

if [ "$MODE" = apply ]; then
  # House rule: an unimplemented command refuses with a non-zero exit and names the
  # phase. A no-op here would look like a successful sync.
  echo "vendor-diff: --apply is not implemented." >&2
  echo "  phase: write. The plan below is what it would do; the three operations" >&2
  echo "  (copy, re-materialise submodules, preserve local files) have no code yet." >&2
  echo "  See issue #25. Run without --apply to see the plan." >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ -n "$HCS_DIR" ]; then
  ref="$HCS_DIR"
  git -C "$ref" rev-parse --verify HEAD >/dev/null 2>&1 || {
    echo "vendor-diff: $HCS_DIR is not a git checkout" >&2; exit 2; }
else
  ref="$work/hcs"
  # Blob-filtered and no checkout: the tree is all this needs, and the vault is large.
  git clone --quiet --filter=blob:none --no-checkout "$HCS_URL" "$ref" 2>/dev/null || {
    echo "vendor-diff: could not clone $HCS_URL (offline?)" >&2; exit 2; }
fi

base="$(git -C "$ref" rev-parse HEAD)"
echo "HCS main: $base"
echo "mirror:   $LOCAL ($(find "$LOCAL" -type f | wc -l) files)"
echo

# Three buckets, then a fourth for what a naive copy would import.
git -C "$ref" ls-tree -r -z "$base" > "$work/tree.z" || exit 2

: > "$work/gitlinks"
: > "$work/regular"
python3 - "$work/tree.z" "$work/gitlinks" "$work/regular" <<'PY'
import sys
tree, links, regular = sys.argv[1], sys.argv[2], sys.argv[3]
with open(tree, 'rb') as fh, open(links, 'w') as lg, open(regular, 'w') as rg:
    for entry in fh.read().split(b'\0'):
        if not entry:
            continue
        meta, path = entry.split(b'\t', 1)
        mode, kind, sha = meta.split()
        p = path.decode()
        if mode == b'160000':
            lg.write(f"{sha.decode()} {p}\n")
        elif kind == b'blob':
            rg.write(f"{sha.decode()} {p}\n")
PY

echo "HCS tree: $(wc -l < "$work/regular") files, $(wc -l < "$work/gitlinks") submodules"
echo

# Build artifacts committed upstream. They are not sources and must not be copied in.
python3 - "$work/regular" "$work/artifacts" "$work/sources" <<'PY'
import sys
src, art, out = sys.argv[1], sys.argv[2], sys.argv[3]
ART = ('.o', '.obj', '.a', '.lib', '.exe', '.dll', '.so', '.pdb')
with open(src) as fh, open(art, 'w') as a, open(out, 'w') as o:
    for line in fh:
        path = line.rstrip('\n').split(' ', 1)[1]
        (a if path.lower().endswith(ART) else o).write(line)
PY

# Classify each source file against the mirror by content, not by presence.
python3 - "$work/sources" "$LOCAL" "$work/add" "$work/update" "$work/same" <<'PY'
import hashlib, os, sys
src, local, add, update, same = sys.argv[1:6]
def blob_sha(path):
    # git blob hash: "blob <len>\0" + content, sha1. Matches ls-tree without shelling out
    # per file, which is 6,600 processes otherwise.
    data = open(path, 'rb').read()
    h = hashlib.sha1()
    h.update(b'blob %d\0' % len(data))
    h.update(data)
    return h.hexdigest()
with open(src) as fh, open(add, 'w') as a, open(update, 'w') as u, open(same, 'w') as s:
    for line in fh:
        sha, path = line.rstrip('\n').split(' ', 1)
        full = os.path.join(local, path)
        if not os.path.exists(full):
            a.write(f"{path}\n")
        elif blob_sha(full) != sha:
            u.write(f"{path}\n")
        else:
            s.write(f"{path}\n")
PY

# A committed binary is not a source either, and it has no extension to give it away:
# HCS main carries `old/srep-main/bin/srep`, a compiled SREP. Sniff the first bytes of
# every would-be new file and move the executables to the artifact bucket.
if [ -s "$work/add" ]; then
  : > "$work/add.bin"
  : > "$work/add.keep"
  while IFS= read -r path; do
    sha="$(awk -v p="$path" '$2 == p { print $1 }' "$work/sources")"
    magic="$(git -C "$ref" cat-file blob "$sha" 2>/dev/null | head -c 4 | od -An -c | tr -d ' ')"
    case "$magic" in
      '177ELF'|'MZ'*|'\312\376\272\276') printf '%s\n' "$path" >> "$work/add.bin" ;;
      *) printf '%s\n' "$path" >> "$work/add.keep" ;;
    esac
  done < "$work/add"
  if [ -s "$work/add.bin" ]; then
    cat "$work/add.bin" >> "$work/artifacts"
    mv "$work/add.keep" "$work/add"
    echo "note: $(wc -l < "$work/add.bin") committed executable(s) classified as artifacts, not sources"
  fi
fi

# Files the mirror has that HCS does not: materialised submodule content and local
# extras. A sync must not delete these.
python3 - "$work/sources" "$LOCAL" "$work/gitlinks" "$work/local_only" <<'PY'
import os, sys
src, local, links, out = sys.argv[1:5]
upstream = set()
with open(src) as fh:
    for line in fh:
        upstream.add(line.rstrip('\n').split(' ', 1)[1])
subroots = []
with open(links) as fh:
    for line in fh:
        subroots.append(line.rstrip('\n').split(' ', 1)[1])
mirror = set()
for root, _, files in os.walk(local):
    for f in files:
        mirror.add(os.path.relpath(os.path.join(root, f), local))
local_only = sorted(mirror - upstream)
with open(out, 'w') as o:
    for p in local_only:
        o.write(p + '\n')
in_sub = sum(1 for p in local_only if any(p == r or p.startswith(r + '/') for r in subroots))
with open(out + '.sub', 'w') as o:
    o.write(str(in_sub))
PY

add_n=$(wc -l < "$work/add")
upd_n=$(wc -l < "$work/update")
same_n=$(wc -l < "$work/same")
art_n=$(wc -l < "$work/artifacts")
loc_n=$(wc -l < "$work/local_only")
sub_n=$(cat "$work/local_only.sub")
links_n=$(wc -l < "$work/gitlinks")

printf '%-46s %s\n' "copy from HCS (new files)"            "$add_n"
printf '%-46s %s\n' "copy from HCS (changed files)"        "$upd_n"
printf '%-46s %s\n' "unchanged"                            "$same_n"
printf '%-46s %s\n' "skip (build artifacts committed upstream)" "$art_n"
printf '%-46s %s\n' "re-materialise submodules (gitlinks)" "$links_n"
printf '%-46s %s\n' "keep (mirror-only files)"             "$loc_n"
printf '%-46s %s\n' "  of which sit under a submodule path" "$sub_n"
echo

for f in add update; do
  n=$(wc -l < "$work/$f")
  [ "$n" -eq 0 ] && continue
  echo "--- $f (first 15 of $n) ---"
  head -15 "$work/$f"
  echo
done

if [ "$art_n" -gt 0 ]; then
  echo "--- build artifacts that a naive copy would import (first 5 of $art_n) ---"
  head -5 "$work/artifacts"
  echo
fi

if [ "$links_n" -gt 0 ]; then
  echo "--- submodules to re-materialise at their pinned SHA ---"
  while read -r sha path; do printf '  %-10s %s\n' "${sha:0:10}" "$path"; done < "$work/gitlinks"
  echo
fi

if [ "$((add_n + upd_n))" -eq 0 ]; then
  echo "in sync: nothing to copy."
  exit 0
fi

echo "stale: $((add_n + upd_n)) file(s) would change. This is a plan, not a sync —"
echo "issue #25 is where the write phase is decided."
exit 1
