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
# `-s subtree --allow-unrelated-histories` "merges well" and then proposes a change
# that deletes every materialised submodule and imports HCS's committed build
# artifacts, because the two trees are not the same shape:
#
#   HCS main          24 gitlinks, committed `.o` files and a compiled `srep`
#   vendor/hcs here   submodule contents materialised as real files, plus local
#                     extras HCS does not have (repackkit/, old/freearc/, ...)
#
# So a tree-level sync in either direction is wrong. `scripts/vendor-diff.sh` counts
# what a correct one consists of — copy, re-materialise, preserve — without touching
# anything; the write phase is issue #25. Counts are deliberately not repeated here:
# they move every time HCS does.
echo "  remedy: none automated — see issue #25. vendor/hcs is a MATERIALISED mirror," >&2
echo "          not a git subtree: 'git subtree pull' refuses here, and forcing it" >&2
echo "          deletes the submodule sources the build compiles." >&2
echo "          What a sync would consist of: ./scripts/vendor-diff.sh" >&2
echo "          compfactory/scripts/bump-hcs.ps1 runs the impossible command" >&2
echo "          (filed as compfactory#8)." >&2
exit 1
