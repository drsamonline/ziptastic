#!/usr/bin/env bash
# check-vendor-current.sh — is the vendored copy still the vault's current copy?
#
# `check-vendor.sh` proves the pins materialise. It cannot notice that the vault
# moved: a copy six months behind passes it, because the sources it pins are
# still there. This checks the other half — that `vendor/hcs/SOURCES` is the same
# file HCS has on `main`. It does not say how to fix it, because there is no one
# command: see the note at the bottom of this file and issue #25.
#
#   ./scripts/check-vendor-current.sh        # exit 0 = current or unverifiable
#
# Network is needed and its absence is not a failure: an offline run reports and
# exits 0, because a transient outage must not look like a stale vault.
set -u

UPSTREAM="${HCS_SOURCES_URL:-https://raw.githubusercontent.com/drsamonline/HCS/main/SOURCES}"
LOCAL="vendor/hcs/SOURCES"

[ -f "$LOCAL" ] || { echo "FAIL: no $LOCAL — is the vendored mirror present?" >&2; exit 1; }

# Fetch to a file, not into a variable: command substitution strips the trailing
# newline, which changes the hash and the line count of every file that ends in
# one — every file, in other words.
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT
curl -fsS --max-time 20 -o "$tmp" "$UPSTREAM" 2>/dev/null || {
  echo "skipped: could not reach $UPSTREAM (offline, or HCS renamed the file)"
  exit 0
}

local_sha="$(sha256sum "$LOCAL" | cut -d' ' -f1)"
remote_sha="$(sha256sum "$tmp" | cut -d' ' -f1)"

if [ "$local_sha" = "$remote_sha" ]; then
  echo "vendor current: SOURCES matches HCS main ($(wc -l < "$LOCAL") lines)"
  exit 0
fi

echo "STALE: vendor/hcs/SOURCES is not HCS main" >&2
echo "  vendored: $local_sha ($(wc -l < "$LOCAL") lines)" >&2
echo "  upstream: $remote_sha ($(wc -l < "$tmp") lines)" >&2
# The remedy used to be printed here as `git subtree pull --prefix=vendor/hcs ...`.
# It cannot run: vendor/hcs was never `git subtree add`ed (one plain copy commit, no
# `git-subtree-dir` marker), so subtree refuses outright. Forcing it with
# `-s subtree --allow-unrelated-histories` "merges well" and then proposes a 543-file,
# -91,112-line change that deletes `vendor/hcs/old/zstd/zstd.h` and every other
# materialised submodule — because the two trees are not the same shape:
#
#   HCS main          24 gitlinks, 262 committed `.o` build artifacts
#   vendor/hcs here   submodule contents materialised as real files, and 247 files
#                     HCS does not have (paq8px/, uc2/src/, srep/, repackkit/)
#
# So a tree-level sync in either direction is wrong: it would import build artifacts
# and delete the sources the build compiles. Syncing means copying the files HCS tracks
# as regular files, re-materialising the 24 submodules at the SHAs the new SOURCES
# pins, and keeping the local extras. Nobody has written that yet, so this gate reports
# drift without pretending there is a one-line fix. Tracked as issue #25.
#
# `compfactory/scripts/bump-hcs.ps1` is not that fix either: it is a four-line wrapper
# around the same `git subtree pull`, so it fails with the same "was never added".
echo "  remedy: none automated — see issue #25. vendor/hcs is a MATERIALISED mirror," >&2
echo "          not a git subtree: 'git subtree pull' refuses here, and forcing it" >&2
echo "          deletes 91k lines the build needs." >&2
echo "          compfactory/scripts/bump-hcs.ps1 runs that same command and fails the" >&2
echo "          same way. A sync must copy HCS's tracked files, re-materialise the 24" >&2
echo "          submodules at the pinned SHAs, and keep the 247 mirror-only files." >&2
exit 1
