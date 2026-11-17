#!/usr/bin/env bash
# vendor-sync.sh — bring vendor/hcs up to HCS main.
#
# `check-vendor-current.sh` says whether the mirror is current; `vendor-diff.sh` says
# what a sync would consist of. This does it.
#
# Why it is not `git subtree pull`: vendor/hcs was copied in by one plain merge commit
# and was never `git subtree add`ed, so subtree refuses. Forcing a tree merge with
# `-s subtree --allow-unrelated-histories` proposes a change that deletes sources the
# build compiles, because the two trees carry different things (HCS commits build
# artifacts; this mirror does not).
#
# What a sync actually is, measured rather than assumed:
#
#   * HCS tracks its sources as 6834 regular files plus 24 gitlinks. ziptastic's
#     vendor/hcs tracks the same shape — 6866 regular files plus 23 gitlinks — and the
#     23 pins are identical to HCS's. So a sync is: copy the regular files that
#     differ, and record the gitlinks that are new or moved.
#   * Submodule *content* is not copied. It is a gitlink; `git submodule update --init`
#     materialises it, which is what CI does with `submodules: recursive`.
#   * Build artifacts HCS commits (`.o` files, and `old/srep-main/bin/srep`, a compiled
#     binary with no extension) are never copied.
#   * Nothing is deleted. Files present here and not upstream stay.
#
#   ./scripts/vendor-sync.sh                # dry run: the plan, changes nothing
#   ./scripts/vendor-sync.sh --apply        # perform it
#   ./scripts/vendor-sync.sh --hcs <dir>    # use a local HCS checkout
#
# Exit 0 = in sync or done, 1 = would change (dry run), 2 = could not run.
set -u

HCS_URL="${HCS_URL:-https://github.com/drsamonline/HCS.git}"
LOCAL="vendor/hcs"
MODE=dry
HCS_DIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) MODE=apply ;;
    --hcs) HCS_DIR="${2:?--hcs needs a path}"; shift ;;
    -h|--help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "vendor-sync: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

[ -d "$LOCAL" ] || { echo "vendor-sync: no $LOCAL — run this from the repository root" >&2; exit 2; }
git rev-parse --verify HEAD >/dev/null 2>&1 || { echo "vendor-sync: not a git checkout" >&2; exit 2; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

if [ -n "$HCS_DIR" ]; then
  ref="$HCS_DIR"
  git -C "$ref" rev-parse --verify HEAD >/dev/null 2>&1 || {
    echo "vendor-sync: $HCS_DIR is not a git checkout" >&2; exit 2; }
else
  ref="$work/hcs"
  git clone --quiet --filter=blob:none --no-checkout "$HCS_URL" "$ref" 2>/dev/null || {
    echo "vendor-sync: could not clone $HCS_URL (offline?)" >&2; exit 2; }
fi

base="$(git -C "$ref" rev-parse HEAD)"
echo "HCS main: $base"
echo

# Upstream: split regular files from gitlinks.
git -C "$ref" ls-tree -r -z "$base" > "$work/up.z" || exit 2
python3 - "$work/up.z" "$work/up.links" "$work/up.files" <<'PY'
import sys
tree, links, files = sys.argv[1:4]
with open(tree, 'rb') as fh, open(links, 'w') as lg, open(files, 'w') as rg:
    for entry in fh.read().split(b'\0'):
        if not entry:
            continue
        meta, path = entry.split(b'\t', 1)
        mode, kind, sha = meta.split()
        p = path.decode()
        (lg if mode == b'160000' else rg).write(f"{sha.decode()} {p}\n")
PY

# Local: the same, from the index rather than the filesystem. The filesystem lies here
# — an uninitialised gitlink's directory can hold anything, including a stray clone,
# and a sync planned from it would try to "preserve" files git does not track.
git ls-tree -r -z HEAD "$LOCAL" > "$work/lo.z" || exit 2
python3 - "$work/lo.z" "$work/lo.links" "$work/lo.files" "$LOCAL" <<'PY'
import sys
tree, links, files, prefix = sys.argv[1:5]
with open(tree, 'rb') as fh, open(links, 'w') as lg, open(files, 'w') as rg:
    for entry in fh.read().split(b'\0'):
        if not entry:
            continue
        meta, path = entry.split(b'\t', 1)
        mode, kind, sha = meta.split()
        p = path.decode()
        if p.startswith(prefix + '/'):
            p = p[len(prefix) + 1:]
        (lg if mode == b'160000' else rg).write(f"{sha.decode()} {p}\n")
PY

# Artifacts upstream: extension, then magic for the extensionless ones.
python3 - "$work/up.files" "$work/artifacts" "$work/sources" <<'PY'
import sys
src, art, out = sys.argv[1:4]
ART = ('.o', '.obj', '.a', '.lib', '.exe', '.dll', '.so', '.pdb')
with open(src) as fh, open(art, 'w') as a, open(out, 'w') as o:
    for line in fh:
        path = line.rstrip('\n').split(' ', 1)[1]
        (a if path.lower().endswith(ART) else o).write(line)
PY

# Regular files that are new or different here.
python3 - "$work/sources" "$work/lo.files" "$work/copy" <<'PY'
import sys
up, lo, out = sys.argv[1:4]
local = {}
with open(lo) as fh:
    for line in fh:
        sha, path = line.rstrip('\n').split(' ', 1)
        local[path] = sha
with open(up) as fh, open(out, 'w') as o:
    for line in fh:
        sha, path = line.rstrip('\n').split(' ', 1)
        if local.get(path) != sha:
            o.write(f"{sha} {path}\n")
PY

# Sniff only the files that would actually be copied: a blob-filtered clone fetches
# each blob on demand, and sniffing all 6,800 upstream files is 6,800 round trips.
: > "$work/artifacts.bin"
: > "$work/copy.final"
while read -r sha path; do
  magic="$(git -C "$ref" cat-file blob "$sha" 2>/dev/null | head -c 4 | od -An -c | tr -d ' ')"
  case "$magic" in
    177ELF*|MZ*) printf '%s %s\n' "$sha" "$path" >> "$work/artifacts.bin" ;;
    *) printf '%s %s\n' "$sha" "$path" >> "$work/copy.final" ;;
  esac
done < "$work/copy"

# Gitlinks that are new or have moved.
python3 - "$work/up.links" "$work/lo.links" "$work/links.change" <<'PY'
import sys
up, lo, out = sys.argv[1:4]
local = {}
with open(lo) as fh:
    for line in fh:
        sha, path = line.rstrip('\n').split(' ', 1)
        local[path] = sha
with open(up) as fh, open(out, 'w') as o:
    for line in fh:
        sha, path = line.rstrip('\n').split(' ', 1)
        if local.get(path) != sha:
            state = 'new' if path not in local else 'moved'
            o.write(f"{sha} {path} {state}\n")
PY

copy_n=$(wc -l < "$work/copy.final")
link_n=$(wc -l < "$work/links.change")
art_n=$(( $(wc -l < "$work/artifacts") + $(wc -l < "$work/artifacts.bin") ))

echo "files to copy:        $copy_n"
echo "gitlinks to record:   $link_n"
echo "artifacts skipped:    $art_n"
echo

if [ "$copy_n" -gt 0 ]; then
  echo "--- files (first 20 of $copy_n) ---"
  head -20 "$work/copy.final" | awk '{print "  " $2}'
  [ "$copy_n" -gt 20 ] && echo "  ..."
  echo
fi
if [ "$link_n" -gt 0 ]; then
  echo "--- gitlinks (pins; content is fetched by 'git submodule update') ---"
  while read -r sha path state; do printf '  %-6s %-10s %s\n' "$state" "${sha:0:10}" "$path"; done < "$work/links.change"
  echo
fi

if [ "$MODE" = dry ]; then
  if [ "$copy_n" -eq 0 ] && [ "$link_n" -eq 0 ]; then
    echo "in sync: nothing to do."
    exit 0
  fi
  echo "dry run: $copy_n file(s) and $link_n gitlink(s) would change. Re-run with --apply."
  exit 1
fi

while read -r sha path; do
  mkdir -p "$LOCAL/$(dirname "$path")"
  git -C "$ref" cat-file blob "$sha" > "$LOCAL/$path" || {
    echo "vendor-sync: could not read blob for $path" >&2; exit 2; }
done < "$work/copy.final"
echo "copied $copy_n file(s)"

while read -r sha path state; do
  # Record the pin. No content is fetched: this tree tracks submodules as gitlinks,
  # exactly as HCS does, and `git submodule update --init` materialises them.
  git update-index --add --cacheinfo "160000,$sha,$LOCAL/$path" || {
    echo "vendor-sync: could not record gitlink $path" >&2; exit 2; }
  mkdir -p "$LOCAL/$path"

  # The vault's own .gitmodules is not the file git reads. Git reads the
  # *superproject's* .gitmodules, with paths relative to this repository, so a
  # gitlink recorded without an entry here fails every recursive checkout with
  # "No url found for submodule path ..." — which is how CI caught the first
  # attempt at this sync. Derive the URL from the vault's entry and add it.
  if ! grep -q "^[[:space:]]*path = $LOCAL/$path\$" .gitmodules 2>/dev/null; then
    url="$(awk -v p="$path" '
      /^\[submodule/ { inside = 1; want = 0; next }
      inside && /^[[:space:]]*path[[:space:]]*=/ { v = $0; sub(/^[^=]*=[[:space:]]*/, "", v); if (v == p) want = 1 }
      inside && want && /^[[:space:]]*url[[:space:]]*=/ { v = $0; sub(/^[^=]*=[[:space:]]*/, "", v); print v; exit }
    ' "$LOCAL/.gitmodules")"
    if [ -z "$url" ]; then
      echo "vendor-sync: no url for $path in $LOCAL/.gitmodules — cannot register it" >&2
      exit 2
    fi
    printf '\n[submodule "%s/%s"]\n\tpath = %s/%s\n\turl = %s\n' \
      "$LOCAL" "$path" "$LOCAL" "$path" "$url" >> .gitmodules
    echo "registered $LOCAL/$path in .gitmodules ($url)"
  fi
done < "$work/links.change"
echo "recorded $link_n gitlink(s)"

echo
echo "vendor/hcs now matches HCS $base."
echo "Nothing was deleted. Submodule content is not in this commit: run"
echo "'git submodule update --init' to materialise the pins."
