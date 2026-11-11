#!/usr/bin/env bash
# check-vendor.sh — fail loudly if a vendored source is missing.
#
# Why this exists: `old/` once carried 23 submodule gitlinks with no
# `.gitmodules`, so every clone saw 23 empty directories and nothing noticed
# until a build or a benchmark tried to use one. Silent emptiness is the failure
# mode; this script makes it loud.
#
# Run it in CI after `git submodule update --init --recursive`, and locally
# before committing anything that touches old/.
#
#   ./scripts/check-vendor.sh [--quiet]
#
# Exit codes: 0 = every source present and accounted for, 1 = something is wrong.
set -u

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

cd "$(dirname "$0")/.." || exit 1

ROOT=old
problems=0

say() { [ "$QUIET" -eq 0 ] && printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; problems=$((problems + 1)); }

[ -d "$ROOT" ] || { echo "FAIL: no $ROOT/ directory — wrong working directory?" >&2; exit 1; }

# --- 1. every directory under old/ must have content ------------------------
present=0
empty=""
for d in "$ROOT"/*/; do
  [ -d "$d" ] || continue
  name=$(basename "$d")
  if [ -z "$(ls -A "$d" 2>/dev/null)" ]; then
    empty="$empty $name"
  else
    present=$((present + 1))
  fi
done

if [ -n "$empty" ]; then
  fail "empty vendored source(s):$empty"
  say "     fix: git submodule update --init --recursive"
fi

# --- 2. every submodule in the index must have a URL ------------------------
# A gitlink with no .gitmodules entry is unrecoverable: git knows the commit
# but not where to get it.
if command -v git >/dev/null && git rev-parse --git-dir >/dev/null 2>&1; then
  gitlinks=$(git ls-tree -r HEAD -- "$ROOT" 2>/dev/null | awk '$1=="160000"{print $4}')
  if [ -n "$gitlinks" ]; then
    [ -f .gitmodules ] || fail "$ROOT has submodule gitlinks but there is no .gitmodules"
    for p in $gitlinks; do
      if ! grep -qE "^[[:space:]]*path[[:space:]]*=[[:space:]]*$p$" .gitmodules 2>/dev/null; then
        fail "gitlink $p has no .gitmodules entry (unfetchable)"
      fi
    done
  fi

  # --- 3. every .gitmodules entry must have a directory and a URL ----------
  if [ -f .gitmodules ]; then
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      [ -d "$p" ] || fail ".gitmodules lists $p but the directory does not exist"
    done < <(grep -E "^[[:space:]]*path[[:space:]]*=" .gitmodules | sed 's/.*=[[:space:]]*//')
  fi
fi

# --- summary ----------------------------------------------------------------
if [ "$problems" -eq 0 ]; then
  say "vendor OK: $present source directories present, all gitlinks mapped"
  exit 0
fi
printf '\n%d problem(s) found.\n' "$problems" >&2
exit 1
