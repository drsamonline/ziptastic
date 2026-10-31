#!/usr/bin/env bash
# check-vendor.sh — fail loudly if a vendored source is missing.
#
# ziptastic vendors a copy of the HCS mirror under vendor/hcs/. 23 of those
# sources are submodules, and for a long time they were gitlinks with no
# `.gitmodules`, which is unrecoverable: every clone silently got 23 empty
# directories, and the first anyone knew was a build or a benchmark reaching for
# one of them.
#
# This checks the copy we actually build against. Run it in CI after checking out
# with submodules, and locally before trusting a build.
#
#   ./scripts/check-vendor.sh [--quiet]
#
# Exit 0 = every vendored source is present and mapped. Exit 1 = something is wrong.
set -u

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

cd "$(dirname "$0")/.." || exit 1

VENDOR=vendor/hcs
ROOT="$VENDOR/old"
problems=0

say() { [ "$QUIET" -eq 0 ] && printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; problems=$((problems + 1)); }

[ -d "$ROOT" ] || { echo "FAIL: no $ROOT/ — is the vendored mirror present?" >&2; exit 1; }

# --- 1. every source directory must have content ----------------------------
present=0
empty=""
for d in "$ROOT"/*/; do
  [ -d "$d" ] || continue
  if [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
    empty="$empty $(basename "$d")"
  else
    present=$((present + 1))
  fi
done

if [ -n "$empty" ]; then
  fail "empty vendored source(s) in $ROOT:$empty"
  say "     fix: git submodule update --init --recursive"
fi

# --- 2. every gitlink must have a URL, and every URL a directory ------------
if command -v git >/dev/null && git rev-parse --git-dir >/dev/null 2>&1; then
  gitlinks=$(git ls-tree -r HEAD -- "$VENDOR" 2>/dev/null | awk '$1=="160000"{print $4}')
  if [ -n "$gitlinks" ]; then
    [ -f .gitmodules ] || fail "the vendored copy has submodule gitlinks but there is no root .gitmodules"
    for p in $gitlinks; do
      if ! grep -qE "^[[:space:]]*path[[:space:]]*=[[:space:]]*$p$" .gitmodules 2>/dev/null; then
        fail "gitlink $p has no .gitmodules entry (unfetchable)"
      fi
    done
  fi

  if [ -f .gitmodules ]; then
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      [ -d "$p" ] || fail ".gitmodules lists $p but the directory does not exist"
    done < <(grep -E "^[[:space:]]*path[[:space:]]*=" .gitmodules | sed 's/.*=[[:space:]]*//')
  fi
fi

if [ "$problems" -eq 0 ]; then
  say "vendor OK: $present source directories present, all gitlinks mapped"
  exit 0
fi
printf '\n%d problem(s) found.\n' "$problems" >&2
exit 1
