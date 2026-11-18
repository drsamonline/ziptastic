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

# --- 4. An .xz stream ------------------------------------------------------------
# Opaque to the engine until 2026-11-07: a .tar.xz cost more through it than the tar did. The
# transform now inflates it and rebuilds the stream from a recipe read out of its own block
# header, so the property to assert is that the layer bit is set AND that the archive beats
# storing the stream.
emit_inline_xz() {
  python3 - "$1" <<'PYEOF'
import lzma, sys
data = (b"the quick brown fox jumps over the lazy dog. " * 4000)
open(sys.argv[1], "wb").write(lzma.compress(data, preset=6))
PYEOF
}
if command -v python3 >/dev/null 2>&1; then
  emit_inline_xz "$TMP/small.tar.xz"
  "$BIN" -9 -f -o "$TMP/xz.dr" "$TMP/small.tar.xz" 2>/dev/null
  "$BIN" -9 --no-precomp -f -o "$TMP/xz_raw.dr" "$TMP/small.tar.xz" 2>/dev/null
  layers3=$(od -An -tu1 -j8 -N1 "$TMP/xz.dr" | tr -d ' ')
  [ "$(( layers3 & 2 ))" -ne 0 ] \
    && ok "xz stream: the layer bit is set" \
    || bad "xz stream: layer bit not set (layers3=$layers3)"
  xz_size=$(stat -c%s "$TMP/xz.dr"); xz_raw=$(stat -c%s "$TMP/xz_raw.dr")
  [ "$xz_size" -lt "$xz_raw" ] \
    && ok "xz stream: $xz_size against $xz_raw opaque" \
    || bad "xz stream: the transform did not help ($xz_size >= $xz_raw)"
  "$BIN" -d -f -o "$TMP/xz.out" "$TMP/xz.dr" 2>/dev/null
  cmp -s "$TMP/small.tar.xz" "$TMP/xz.out" \
    && ok "xz stream: byte-exact" || bad "xz stream: round trip differs"
else
  echo "  (skipped the xz case: no python3 to build the fixture)"
fi

# --- 5. A near-duplicate tree ---------------------------------------------------
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

# --- 6. A valid 16-bit PCM WAV --------------------------------------------------
# The bug was not in the transform. It was in the trial that decides whether to keep
# it: the trial re-entered compress() with the wav stage still enabled, so it
# contained itself and every valid WAV of 256 bytes or more died with SIGSEGV.
#
# Every fixture in this file that is *malformed* passes either way, because parse()
# rejects it and the transform never runs. So the fixture here is valid by
# construction, and the three properties asserted are the ones that were broken: the
# CLI exits 0 (it used to die on SIGSEGV), the round trip is byte-exact, and the
# stage is worth something against --no-wav.
emit tone.wav "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"
"$BIN" -9 -f -o "$TMP/tone.dr" "$TMP/tone.wav" 2>/dev/null
[ $? -eq 0 ] \
  && ok "valid wav: exit 0" \
  || bad "valid wav: the CLI did not exit 0"
"$BIN" -d -f -o "$TMP/tone.out" "$TMP/tone.dr" 2>/dev/null
cmp -s "$TMP/tone.wav" "$TMP/tone.out" \
  && ok "valid wav: byte-exact" || bad "valid wav: round trip differs"
"$BIN" -9 -f --no-wav -o "$TMP/tone.nowav.dr" "$TMP/tone.wav" 2>/dev/null
with_wav=$(stat -c%s "$TMP/tone.dr")
without_wav=$(stat -c%s "$TMP/tone.nowav.dr")
[ "$with_wav" -lt "$without_wav" ] \
  && ok "valid wav: the stage pays ($with_wav of $without_wav bytes)" \
  || bad "valid wav: the stage bought nothing ($with_wav of $without_wav bytes)"

# --- 7. A gzip of a CSV, where inflating the member pays -------------------------
# The ablation of FINDINGS section 2 recorded "gzip members: did not fire" from a single
# fixture, and that was one file standing in for a content class: on four fixtures the
# transform fires on three. This case holds the property that matters — a `.gz` holding
# tabular data is worth inflating — so a change to the gate cannot quietly stop doing it.
emit small.csv.gz "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"
"$BIN" -9 -f -o "$TMP/csv.dr" "$TMP/small.csv.gz" 2>/dev/null
"$BIN" -9 --no-precomp -f -o "$TMP/csv.off.dr" "$TMP/small.csv.gz" 2>/dev/null
# layers is byte 6 of the stream (after "CHCE", version and flags); gzip is bit 0x20 of it.
csv_layers=$(od -An -tu1 -j6 -N1 "$TMP/csv.dr" | tr -d ' ')
[ "$(( csv_layers & 32 ))" -ne 0 ] \
  && ok "gzip of a CSV: the member is inflated (layers=$csv_layers)" \
  || bad "gzip of a CSV: the transform did not fire (layers=$csv_layers)"
with_gz=$(stat -c%s "$TMP/csv.dr")
without_gz=$(stat -c%s "$TMP/csv.off.dr")
[ "$with_gz" -lt "$without_gz" ] \
  && ok "gzip of a CSV: inflating pays ($with_gz of $without_gz bytes)" \
  || bad "gzip of a CSV: inflating bought nothing ($with_gz of $without_gz bytes)"
"$BIN" -d -f -o "$TMP/csv.out" "$TMP/csv.dr" 2>/dev/null
cmp -s "$TMP/small.csv.gz" "$TMP/csv.out" \
  && ok "gzip of a CSV: round trip exact" \
  || bad "gzip of a CSV: round trip differs"

# --- 8. An LZ4 frame from this build's own library -------------------------------
# The transform stores no parse: it rebuilds the frame by re-encoding and comparing, and
# DECLINES when the producing liblz4 is a different version (the matcher changed in
# 1.10.1). Both halves matter — a frame it can reproduce must be salvaged, and one it
# cannot must be left alone rather than approximated. unit.lz4 covers the decline; this
# covers the salvage, through the CLI and the layer bit.
emit small.lz4 "BCJNGGhAShkAAAAAAADqixUAAPBbYSxiLGMKMCwwLDAKMSw3LDEKMiwxNCwyCjMsMjEsMwo0LDI4LDQKNSwzNSw1CjYsNDIsNgo3LDQ5LDcKOCw1Niw4CjksNjMsOQoxMCw3MCwxMAoxMSw3NywxMQoxMiw4NCwxMgoxMyw5MWEA8G00LDk4LDEKMTUsMTA1LDIKMTYsMTEyLDMKMTcsMTE5LDQKMTgsMTI2LDUKMTksMTMzLDYKMjAsMTQwLDcKMjEsMTQ3LDgKMjIsMTU0LDkKMjMsMTYxLDEwCjI0LDE2OCwxMQoyNSwxNzUsMTIKMjYsMTgyLDAKMjcsMTg52wDwdzgsMTk2LDIKMjksMjAzLDMKMzAsMjEwLDQKMzEsMjE3LDUKMzIsMjI0LDYKMzMsMjMxLDcKMzQsMjM4LDgKMzUsMjQ1LDkKMzYsMjUyLDEwCjM3LDI1OSwxMQozOCwyNjYsMTIKMzksMjczLDAKNDAsMjgwLDEKNDEsMjg3LDIKNDIsMjk0VwHw+DMsMzAxLDQKNDQsMzA4LDUKNDUsMzE1LDYKNDYsMzIyLDcKNDcsMzI5LDgKNDgsMzM2LDkKNDksMzQzLDEwCjUwLDM1MCwxMQo1MSwzNTcsMTIKNTIsMzY0LDAKNTMsMzcxLDEKNTQsMzc4LDIKNTUsMzg1LDMKNTYsMzkyLDQKNTcsMzk5LDUKNTgsNDA2LDYKNTksNDEzLDcKNjAsNDIwLDgKNjEsNDI3LDkKNjIsNDM0LDEwCjYzLDQ0MSwxMQo2NCw0NDgsMTIKNjUsNDU1LDAKNjYsNDYyLDEKNjcsNDY5LDIKNjgsNDc2LDMKNjksNDgzLDQKNzAsNDkwLDUKNzEsNDk3TQLwEDIsNTA0LDcKNzMsNTExLDgKNzQsNTE4LDkKNzUsNTIsAnAKNzYsNTMyVQLwLDc3LDUzOSwxMgo3OCw1NDYsMAo3OSw1NTMsMQo4MCw1NjAsMgo4MSw1NjcsMwo4Miw1NzQsNAo4Myw1FgDwAQo4NCw1ODgsNgo4NSw1OTXHAvACNiw2MDIsOAo4Nyw2MDksOQogACAxNmgBYDg5LDYyM3gAYDkwLDYzMHgA8Bk5MSw2MzcsMAo5Miw2NDQsMQo5Myw2NTEsMgo5NCw2NTgsMwo5NSw2GAFQCjk2LDbiAPEUCjk3LDY3OSw2Cjk4LDY4Niw3Cjk5LDY5Myw4CjEwMCw3MDBEA1AxLDcwN3oAcDEwMiw3MTR7AHAxMDMsNzIxfAAQMSoBIDI4TANgMDUsNzM1TgNAMDYsN1oCYAoxMDcsNyUCgAoxMDgsNzU2UQNgMDksNzYzUgMAogNhNzAsNgoxowNANyw3Cn4DMDc4NIUAAPwBIDkxhQBAMTQsN6gDkTAKMTE1LDgwNYUAYTE2LDgxMoUAYDE3LDgxOYUAYDE4LDgyNoUAQDE5LDgzA4AKMTIwLDg0MNUDYDIxLDg0N4UAYDIyLDg1NIUAYDIzLDg2MYUAYDI0LDg2OIUAYDI1LDg3NYUAYDI2LDg4MoUAQDI3LDi1A4AwCjEyOCw4OSoEgQoxMjksOTAzhQBRMzAsOTG3BFAzMSw5MbsEYDEzMiw5Mr4EADsEIDkzwQRgMTM0LDkzxARgMTM1LDk0xwQQMU4DEDXKBGAxMzcsOTXNBGAxMzgsOTbQBGExMzksOTfTBEA0MCw5wgOAMAoxNDEsOTimBIEKMTQyLDk5NIUAAY4DIDAxCwEACgIhMDDfBBA0tAIRMeEEEDRHAhEy4wQQNNgBETLlBAA5AyEwM+cEYDQ5LDEwNOkEMDE1MBcFAOsEIDE11gQQNe0EETFpBCA2NBQBAOUCEDDaA0AwCjE1kAMRN/MEIDE1cAAROPUEIDE1cQAQOfcEIDE1cQAgOTmeARA1KQAQMPsEETGvBCAxM6ABEDZCBCAyMKEBEDbVAxAyAQUgMTZoAxAzAwUgMTb7AiA0MaQBEDaLAhA0BwUgMTYRAiA1NZIAEDaXAQAxAEAwCjE2HgEQNl4AEgqMBRE3DwUCEwAQOBEFIDE3cQAgOTCSAADHACAxORUFIDE3VgIgMDSSABA33AEQMRkFIDE3YgEQMRsFAtAFIDI1NQIBVAAgMzKSAABlBiAyMyEFEDEIASAyNCMFIDE3HAQBKQEgCjG7AREy5QAwCjE4QAMAowAgMgoWBjAxMjcrBQCYACAxMhwAMAoxOHEAEDgvBRExTAEgOTWSAGA4NiwxMzAzBRAx/wEwMzA5xgJgODgsMTMxNwUQMaUCMDMyM5IAANoAIDMzOwUwMTkxvwYQNyQBUTkyLDEzHgJwCjE5MywxM90BMDEKMUMCEDOZAaAyCjE5NSwxMzY1SAIA6gIQMxMBEAofAUAxMzc5KQcAkAMwMzg2kgAA0gGBMzkzLDQKMjAcB2AwLDUKMjAeByEwNzEHAMMHEDFTBQDlBiAxNIcDEAphATAxNDJXBRAy0gMQNDwEkDAKMjA2LDE0NPMBIAoyegQQNJECMDIKMsoCIDQ1XwVgMjA5LDE0CwIQCiwHMDE0N2MFAKYBMDE0N2UFEDIXBCA0OGcFgDIxMywxNDkxkgAAvQQQNO4EcAoyMTUsMTWxBCAKMlgFEDUaCBAKcAcwMTUxcQUA6AEgMTVJBDAwCjL6ByA1M3oCQAoyMjDrBwF3BRAyAgUgNTR5BWAyMjIsMTUCAyAKMp4FIDU2fQVwMjI0LDE1Nn8FAC0CQDE1NzUkAQBWABA1CAZwCjIyNywxNdMFIAoyCwggNTmHBXAyMjksMTYwiQUQMuEFMTYxMFsIQDEsMTZWBCAwCnACIDE2XgAwMQoymQAQNh0AkDIKMjM0LDE2M44FEDJOARA2+AOQCjIzNiwxNjUyZwggMzcLAEA5LDMKQgggMTYiBxAKtAIgMTbuBiAKMtsAMDY4MLYBEDRxABA4lQUQMo8BIDY5lgUQMmEEMDcwMZIAAEMCEDeXASAwCocIIDE3VgEgMQr4AiAxNxIBMDIKMmQEIDcymwUQMmQEEDeMACAKMtMBMDc0M5IAAGQEEDc+CCAKMocCIDc1nwUBzQggNzagBQE7AzA3NzGSAABhATA3Nzh7CQBiBHA3ODUsOAoyYQQwNzkykgAAYQQQN48CMDAKMskCEDhNAiExChIJEDgKAiEyCn8DIDgyqAUQMmQEEDiEASAKMjMEEDhZCSAKMlkCMDg0MSQBAGQEIDg0rAUQMmQEIDg1rQUBVgkwODYykgAAwAMgODavBXAyNjgsMTg3sAUQMnQEIDg4sQUQMmEEEDiHAwD2CRAx9wkAlQQgCjJQAyA5MOEDIAoyZAQgOTG0BQAEBCAxOXMKgAoyNzUsMTkytAUQMmQEIDkztAUQMmQEIDkztAUQMmQEMDk0NtoCAGQEIDk1tAUQMmMEIDk2tAUBRgQgOTa0BXAyODIsMTk3tAVgMjgzLDE5YQQwMAoyYgQQOTsEMDEKMmMEEDn3A5AyCjI4NiwyMDC0BQEnChAwcQMRCosEIDAxtAVwMjg5LDIwMrQFMDI5MLsKALQFMDI5MQsAALQFcDI5MiwyMDS0BYAyOTMsMjA1MbYBIDk0CwAQOLYBYDk1LDIwNrQFEDICCxAwEQEwMAoygQUQMM8AgDEKMjk4LDIw7wQAIAsAcQAQObQFITMwIwsAtAUAsgoQMmQIIDIKEgUQMikIAEMLACsMIDIxRQtAMDQsMq8HMDUKM+ALAHIHMDYKM3gGADUHcDcKMzA3LDLzBiA4CvYKEDKuBiA5ClQFETJsBkAwCjMxbwAAxwFBMQozMZULAd0KUDMxMiwyowUAIwAAcQAAYQUgCjOrDAAcBSAyCj0LEDLaBCAzCpkFEDKVBADXCxA3CwAAtAVwMzE4LDIyMrQFUDMxOSwyygNwNwozMjAsMocDUDgKMzIxCwAAtAUAgQsRMgADcTAKMzIzLDK8AjAxCjNdCAF7AjAyCjMtAgA3AgAjADE2LDL1ATAKMzJxAACwATAKMzJxABA5tAUAyAtAMjMwM5IAAHMMAOYAMDUKM3UMIDMxtAUQM3cMAF4AMDcKM0YJABsAMDgKMyIDEDOZCCAKM30MEDNhBIAwCjMzNiwyMxgIUDEKMzM3DAAA9QYQCioAMDIzNrQFYDMzOSwyM+oCQAozNDDADAC0BTAzNDELAAC0BRAzMQogMzm0BQBTDEAyNDAxkgAgNDQLAAAfCwBsAEAyNDE1kgBQNDYsMjTDCUAKMzQ3CwAAtAVgMzQ4LDI0YQSAMAozNDksMjSsBIAxCjM1MCwyNGgEIDIK6wYwMjQ1tAUAsAAgMjTiA4AKMzUzLDI0N7QFIDM1cQAQN7QFIDM1Xw0QOLQFIDM1cQAgOTKSABA1cQAAKwsQCiwHIDI17QqACjM1OSwyNTG0BRAz8QsgNTK0BRAzBwYQNWEEgDAKMzYyLDI1oQWAMQozNjMsMjVgBSAyCiINMDI1NLQFAHAHIDI12gQQCjUBIDI1MwAwCjM20w0QNrQFcDM2OCwyNTdGCyAzNnEAAEUMIAoz/wYQNQ8MIAozxAAgNTm0BXAzNzIsMjYwtAUAdwFAMjYxMdoCIDc0CwAQONELcTM3NSwyNjJNC2AzNzYsMjbRDFAyCjM3NwwAALQFAa0NEDYzChEK9wcQNikBEQq8ATA2NjDaAlA4MSwyNl8NMAozOHEAEDe0BXAzODMsMjY4tAUgMzhxACA4OLYBEDhwACA5NUgCAGEEIDcwtAVgMzg3LDI3YQSAMAozODgsMjeRBzAxCjOzAiA3Mn8JIAozZAQwNzMw2gIAZAQQN8cGEAo4DiAyNyECEAp+CEAyNzUxkgAAZAQgNzW0BRAzZAQgNza0BRAzZAQwNzcy/gMAYwQwNzc5kgAAYgQgNzi0BRAzYQQgNzm0BRA0YQQgODD8ChAKhgIgMjiHCJExCjQwMiwyODG0BXA0MDMsMjgytAUQNGQEEDi/ByAKNGQEEDgZAxAKww5AMjg0MlsPAGQEIDg0tAVwNDA4LDI4NbQFcDQwOSwyODa0BRA0YwQwODcwYw8AYgQgODe0BRA0YQQgODi0BSA0MW4AAGEEIDAKSAkgMjh+CSAxCg4DIDI5PQkwMgo0mQUgOTG0BWA0MTcsMjm2CCAKNGQEIDkytAUQNGQEMDkzM5IAAGQEMDk0MO8PAGQEIDk0tAUAUQMwMjk1tAUQNGMEIDk2tAUQNGIEIDk2tAUQNGEEIDk3tAUQNFMAEDlWBjAwCjRiBBA5dgogMQrQCSAyOTIKkDIKNDI5LDMwMLQFMDQzMF8QALQFEDQFDRAwogAgCjTTBjAwMjSSAACYACAwM7QFANkPMDMwM2gLcDQzNSwzMDRoCwDYA0AzMDUyJAEgMzcLAABoC3A0MzgsMzA2aAsgNDNuAABhBDAwCjTuDRAwCgcgMQoeECAzMCoLIDIKVwowMzA5aAsAHARAMzEwMQ0RIDQ0CwAAaAsRNPUQEDFoCxA09xAwMTIyJAEA+RAA6g0wNQo0+xAArg0gNgqYChAzcA0wNwo0wAgALg0wOAo0hQIgMTW0BVE0NTIsM6cMMDAKNAMRAmMMMAo0NXAAAL4HIDIKqBAQM94LACMAMTYsM5wLEAqhBBAzVwsAIQBQOCwzMjC0BRA0uAkA0AowNAo0fQMwMjIwkgAgNjGfEQC0BQDsEBAzBAogNwoeCxAzwgkgOArjBBAzfQlxOQo0NjUsMzsJQDAKNDbXEQD3CDAxCjSbEAG2CDAyCjSwCgByCAAjADE5LDMwCBAKYwsRM+sHEAooBRAzqAdwMwo0NzIsM2QHcDQKNDczLDMgB1A1CjQ3NAsAALQFUDQ3NSwzmAYAPRIwNiwzVgYhOAqlCwARBiE5CmoFAM8FAJIAUDc5LDMzKg1QMQo0ODBjEgFoCzA0ODEMAAC0BSA0OHEAAIkNEAq+EREzfwQQCuoLEDM8BCAzCq8FEDP4AzA0CjSdDAC0AzA1CjRiBgBxA3A2CjQ4OCwzLANwNwo0ODksM+oCUDgKNDkwyRIAtAUALAwQM2MCAJIAQDkyLDMfAgCSAEA5Mywz3gEAJAEAMxMgNDW0BRA0lQ0AWAEwMQo0WgcAEwEgMgpJEhEz0AAQCnEMEDOMAEA0CjQ5cQAgOTPlEhEwMBMAaAsgNTAxEwApEYAKNTAyLDM1MbQFEDUOFCA1MrQFAI0SIDM1YQQgMAq0DCAzNdwLIDEKegYwMzU0rA4QCkAAMDM1NLQFYDUwOCwzNRULcAo1MDksMzVtBjAKNTFxABA3tAUA1BJAMzU3N0YVAOwUIDU4tAUAvQYwMzU5tAUAggCAMzU5OCw3CjUMAyA2MLQFcDUxNiwzNjG0BTA1MTcLAAApEHAKNTE4LDM21AwgMQo8DSAzNpEMkDIKNTIwLDM2NLQFAMcAIDM2CgwgCjV6EBA2ZAcgCjU/ChA2vgIgCjUEBCA2NrQFcDUyNSwzNje0BQB/DTAzNji0BQBEB0AzNjg5kgAQMnAAEDm0BRA1YQQgNzC0BRA1YQQQN2EEAG4UEDFvFAFoCwCkEyAzNyoNIDIKww0wMzcztAUgNTOJFBAzHBEATQEwMzc0aAtgNTM2LDM3tgMgCjVkBCA3NbQFEDVkBCA3NrQFAOgTgDM3NzMsNgo1YwQgNzi0BQDJBzAzNzi0BQCOATAzNzm0BRA1UhUQOGEEMDAKNWIEEDgqDTAxCjVjBBA4Kg0gMgouFDAzODK0BQBIDiAzOPoNEAoNCCAzOFMJEArSASAzOEoAIAo1ZAQgODW0BRA1ZAQwODU3SAIArgAwODY0kgAAYwQgODe0BQCKDjAzODe0BQBPCHAzODg1LDkKFAIgMzhhBDAwCjVvFRA4Kg0wMQo1YwQQOSoNAJgVUDksMzkxtAUQNWQEIDkytAUAzw4wMzkyHBEAlAggMzk/ARAKWQIwMzk0tAUwNTY0CwAAtAVwNTY1LDM5NbQFEDVjBCA5NrQFEDViBCA5NrQFEDVhBCA5N7QFEDVhBBA5kQwhMAqcAhA5Kg1QMQo1NzENFgFoC3A1NzIsNDAwtAVwNTczLDQwMbQFAUQVEDDdBhEKVg8gMDK0BQEbCSAwM7QFAeACIDAzaAuANTc4LDQwNDYkAXA3OSw0MDUzSAJgODAsNDA2HBEAiBUwNDA2tAUAmA8gNDBhBCAwCl4JIDQwKg0gMQokAyA0MCoNMDIKNRMKIDA5HBFQNTg2LDQZFQAiABA3CwAAHBEAzxUwNDExtAWANTg5LDQxMjNsA0A5MCw0JRQgNQpnAxA06hMA5RYwMiw0qxMwNwo1QxEAaRMwOAo1CAswMTU4tgFBOTUsNOISIDAKIBAgNDEqDSAxCuYJEjRdEhAKrAMwNDE4aAuwNTk5LDQxOTMsMQoAAAAA"
"$BIN" -9 -f -o "$TMP/lz4.dr" "$TMP/small.lz4" 2>/dev/null
"$BIN" -9 --no-lz4 -f -o "$TMP/lz4.off.dr" "$TMP/small.lz4" 2>/dev/null
# layers3 is byte 8; kLayer3Lz4 is bit 0x10 of it.
lz4_layers3=$(od -An -tu1 -j8 -N1 "$TMP/lz4.dr" | tr -d ' ')
[ "$(( lz4_layers3 & 16 ))" -ne 0 ] \
  && ok "lz4 frame: kLayer3Lz4 is set (layers3=$lz4_layers3)" \
  || bad "lz4 frame: the transform did not fire (layers3=$lz4_layers3)"
with_lz4=$(stat -c%s "$TMP/lz4.dr")
without_lz4=$(stat -c%s "$TMP/lz4.off.dr")
[ "$with_lz4" -lt "$without_lz4" ] \
  && ok "lz4 frame: salvage pays ($with_lz4 of $without_lz4 bytes)" \
  || bad "lz4 frame: salvage bought nothing ($with_lz4 of $without_lz4 bytes)"
"$BIN" -d -f -o "$TMP/lz4.out" "$TMP/lz4.dr" 2>/dev/null
cmp -s "$TMP/small.lz4" "$TMP/lz4.out" \
  && ok "lz4 frame: round trip exact" \
  || bad "lz4 frame: round trip differs"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
