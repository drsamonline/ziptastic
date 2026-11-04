#!/usr/bin/env bash
# regression-corpus.sh — the fixtures that caught real bugs, run as a test.
#
# Every case here is a file that was once handed to the backend as opaque bytes, or was
# once made larger by a transform that was supposed to shrink it. Each one asserts the
# property that was broken, not just that the round trip works — a round trip passes
# whether or not the transform fired, which is exactly how these went unnoticed.
#
# The fixtures are base64 in this file rather than binaries in the tree: the commit path
# is text-only UTF-8, so a checked-in .zip would be silently skipped.
#
# Run directly, or through ctest as integration.corpus:
#   ZIPT_BIN=./build/ziptastic bash scripts/regression-corpus.sh
set -u

BIN="${ZIPT_BIN:?ZIPT_BIN must point at the ziptastic binary}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok()   { pass=$((pass + 1)); printf '[ ok ] %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '[FAIL] %s\n' "$1"; }

emit() { printf '%s' "$2" | base64 -d > "$TMP/$1"; }

# The flags byte is the sixth byte of the stream: "CHCE", version, then flags.
flags() { od -An -tu1 -j5 -N1 "$1" | tr -d ' '; }

# --- 1. A ZIP written to a pipe -------------------------------------------------
# Data descriptors: the local headers carry zero sizes and the real ones follow the data.
# scan_zip read only local headers, so every member was skipped and the file went to the
# backend unchanged. 320,316 bytes before the fix, 183,286 after.
emit streamed.zip "UEsDBBQACAAIADo/ZF0AAAAAAAAAAAAAAAAFAAAAYS50eHQryUhVKCzNTM5WSCrKL89TSMuvUMgqzS0oVsgvSy1SKAFK5yRWVSqk5KfrgXmjikcVjyoeVTyqeFTxMFIMAFBLBwiku3b9PgAAAAgHAABQSwMEFAAIAAgAOj9kXQAAAAAAAAAAAAAAAAUAAABiLnR4dFPQS89PUaisSsxRSM0oUShKLctXKC7ILc1SqMhPU8grzy9KUshOziwtBEuPKh5VPKp4VPGo4lHFw0sxAFBLBwji9zkTPAAAAAgHAABQSwECFAMUAAgACAA6P2RdpLt2/T4AAAAIBwAABQAAAAAAAAAAAAAAgAEAAAAAYS50eHRQSwECFAMUAAgACAA6P2Rd4vc5EzwAAAAIBwAABQAAAAAAAAAAAAAAgAFxAAAAYi50eHRQSwUGAAAAAAIAAgBmAAAA4AAAAAAA"
"$BIN" -9 -f -o "$TMP/streamed.dr" "$TMP/streamed.zip" 2>/dev/null
[ "$(( $(flags "$TMP/streamed.dr") & 4 ))" -ne 0 ] \
  && ok "streamed zip: precomp fires" \
  || bad "streamed zip: precomp did not fire (flags=$(flags "$TMP/streamed.dr"))"
"$BIN" -d -f -o "$TMP/streamed.out" "$TMP/streamed.dr" 2>/dev/null
cmp -s "$TMP/streamed.zip" "$TMP/streamed.out" \
  && ok "streamed zip: byte-exact" || bad "streamed zip: round trip differs"

# --- 2. The same ZIP inside a plain tar ----------------------------------------
# This is the case FINDINGS.md used to blame on the 512-byte tar headers. The tar was
# never the obstacle: the member inside it was streamed, so the scan skipped it.
emit streamed.tar "aW5uZXIuemlwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDY0NCAAMDAwMDAwIAAwMDAwMDAgADAwMDAwMDAwNTM0IDAwMDAwMDAwMDAwIDAwNzYyNgAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABQSwMEFAAIAAgAOj9kXQAAAAAAAAAAAAAAAAUAAABhLnR4dCvJSFUoLM1MzlZIKsovz1NIy69QyCrNLShWyC9LLVIoAUrnJFZVKqTkp+uBeaOKRxWPKh5VPKp4VPEwUgwAUEsHCKS7dv0+AAAACAcAAFBLAwQUAAgACAA6P2RdAAAAAAAAAAAAAAAABQAAAGIudHh0U9BLz09RqKxKzFFIzShRKEoty1coLsgtzVKoyE9TyCvPL0pSyE7OLC0ES48qHlU8qnhU8ajiUcXDSzEAUEsHCOL3ORM8AAAACAcAAFBLAQIUAxQACAAIADo/ZF2ku3b9PgAAAAgHAAAFAAAAAAAAAAAAAACAAQAAAABhLnR4dFBLAQIUAxQACAAIADo/ZF3i9zkTPAAAAAgHAAAFAAAAAAAAAAAAAACAAXEAAABiLnR4dFBLBQYAAAAAAgACAGYAAADgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
"$BIN" -9 -f -o "$TMP/tar.dr" "$TMP/streamed.tar" 2>/dev/null
[ "$(( $(flags "$TMP/tar.dr") & 4 ))" -ne 0 ] \
  && ok "tar of a streamed zip: precomp fires" \
  || bad "tar of a streamed zip: precomp did not fire (flags=$(flags "$TMP/tar.dr"))"
"$BIN" -d -f -o "$TMP/tar.out" "$TMP/tar.dr" 2>/dev/null
cmp -s "$TMP/streamed.tar" "$TMP/tar.out" \
  && ok "tar of a streamed zip: byte-exact" || bad "tar of a streamed zip: round trip differs"

# --- 3. A small PNG -------------------------------------------------------------
# The regression: a 1.2 KB UI-style PNG encoded to 336 bytes with the transform and 309
# without, because on a file that small the block framing is a large share of the total.
# The property that was fixed is an invariant, so it is what gets asserted: with the
# transform can never be larger than without it.
emit ui.png "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"
"$BIN" -9 -f -o "$TMP/png.dr" "$TMP/ui.png" 2>/dev/null
"$BIN" -9 --no-precomp -f -o "$TMP/png_raw.dr" "$TMP/ui.png" 2>/dev/null
with=$(stat -c%s "$TMP/png.dr")
without=$(stat -c%s "$TMP/png_raw.dr")
[ "$with" -le "$without" ] \
  && ok "small PNG: the transform does not make it bigger ($with <= $without)" \
  || bad "small PNG: transform made it bigger ($with > $without)"
"$BIN" -d -f -o "$TMP/png.out" "$TMP/png.dr" 2>/dev/null
cmp -s "$TMP/ui.png" "$TMP/png.out" && ok "small PNG: byte-exact" || bad "small PNG: round trip differs"

# --- 4. A near-duplicate tree ---------------------------------------------------
# The shape a game ships: the same asset a thousand times with a few bytes changed. The
# match finder and the dedup stage are what handle it, and the archive should be a
# fraction of the input rather than a copy of it.
head -c 4096 /dev/urandom > "$TMP/block.bin"
: > "$TMP/dups.bin"
i=0
while [ $i -lt 1000 ]; do
  cat "$TMP/block.bin" >> "$TMP/dups.bin"
  printf '%08d' "$i" >> "$TMP/dups.bin"
  i=$((i + 1))
done
"$BIN" -9 -f -o "$TMP/dups.dr" "$TMP/dups.bin" 2>/dev/null
in_size=$(stat -c%s "$TMP/dups.bin")
out_size=$(stat -c%s "$TMP/dups.dr")
[ "$(( out_size * 20 ))" -lt "$in_size" ] \
  && ok "near-duplicate tree: $out_size of $in_size bytes (under 5%)" \
  || bad "near-duplicate tree: $out_size of $in_size bytes, expected under 5%"
"$BIN" -d -f -o "$TMP/dups.out" "$TMP/dups.dr" 2>/dev/null
cmp -s "$TMP/dups.bin" "$TMP/dups.out" \
  && ok "near-duplicate tree: byte-exact" || bad "near-duplicate tree: round trip differs"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
