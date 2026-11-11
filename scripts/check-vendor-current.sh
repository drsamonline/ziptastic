#!/usr/bin/env bash
# check-vendor-current.sh — is the vendored copy still the vault's current copy?
#
# `check-vendor.sh` proves the pins materialise. It cannot notice that the vault
# moved: a copy six months behind passes it, because the sources it pins are
# still there. This checks the other half — that `vendor/hcs/SOURCES` is the same
# file HCS has on `main` — and says the one command that fixes it.
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
echo "  fix: git subtree pull --prefix=vendor/hcs https://github.com/drsamonline/HCS.git main --squash -m 'vendor: sync HCS'" >&2
echo "       (or run compfactory/scripts/bump-hcs.ps1)" >&2
exit 1
