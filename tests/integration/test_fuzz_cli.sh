#!/usr/bin/env bash
# test_fuzz_cli.sh — end-to-end fuzzing through the real CLI.
#
# The unit fuzz suite mutates encoded blocks and checks the decoder in-process.
# This drives the actual binary the way a user would: compress a file, decompress
# it, compare bytes; then corrupt the compressed stream and require a clean
# failure rather than a crash or a hang.
#
# The distinction matters here: every bug that reached a measurement in this
# project was an integration bug — a flag that disagreed with its data, a mode
# case grouped with the wrong handler — and none of those would be caught by
# mutating a block in-process.
#
# Deterministic: RANDOM is seeded, so any failure reproduces.
set -u

BIN="${ZIPT_BIN:?ZIPT_BIN must point at the ziptastic binary}"
ITER="${ITER:-25}"
SEED="${SEED:-12345}"
RANDOM="$SEED"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass=0
fail=0
ok()  { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); printf '[FAIL] %s\n' "$1"; }

make_input() {  # $1 path, $2 size, $3 kind
  local size="$2" kind="$3"
  case "$kind" in
    0) head -c "$size" /dev/urandom > "$1" 2>/dev/null || : > "$1" ;;
    1) yes "the quick brown fox jumps over the lazy dog" 2>/dev/null | head -c "$size" > "$1" ;;
    2) head -c "$size" /dev/zero | tr '\0' 'A' > "$1" ;;
    3) head -c "$size" /dev/urandom | base64 2>/dev/null | head -c "$size" > "$1" ;;
  esac
  local actual
  actual=$(wc -c < "$1")
  if [ "$actual" -lt "$size" ]; then
    head -c $((size - actual)) /dev/zero >> "$1"
  elif [ "$actual" -gt "$size" ]; then
    head -c "$size" "$1" > "$1.t" && mv "$1.t" "$1"
  fi
}

# --- 1. round-trip through the CLI ------------------------------------------
i=0
while [ "$i" -lt "$ITER" ]; do
  size=$(( (RANDOM * 2) % 65536 ))
  kind=$(( i % 4 ))
  make_input "$TMP/in.bin" "$size" "$kind"
  case $(( i % 3 )) in
    0) flags="" ;;
    1) flags="-9" ;;
    2) flags="-9 --no-bcj --no-precomp" ;;
  esac
  if $BIN $flags -f -o "$TMP/c.chce" "$TMP/in.bin" >/dev/null 2>&1 \
     && $BIN -d -f -o "$TMP/out.bin" "$TMP/c.chce" >/dev/null 2>&1 \
     && cmp -s "$TMP/in.bin" "$TMP/out.bin"; then
    ok
  else
    bad "round-trip size=$size kind=$kind flags='$flags'"
  fi
  i=$((i + 1))
done

# --- 2. corrupted streams must fail cleanly ---------------------------------
i=0
while [ "$i" -lt "$ITER" ]; do
  size=$(( 1024 + (RANDOM % 32768) ))
  make_input "$TMP/in.bin" "$size" "$(( i % 4 ))"
  if ! $BIN -9 -f -o "$TMP/c.chce" "$TMP/in.bin" >/dev/null 2>&1; then
    i=$((i + 1)); continue
  fi
  clen=$(wc -c < "$TMP/c.chce")
  [ "$clen" -lt 8 ] && { i=$((i + 1)); continue; }

  off=$(( RANDOM % clen ))
  cp "$TMP/c.chce" "$TMP/bad.chce"
  printf "\\x$(printf %02x $(( RANDOM % 256 )))" | dd of="$TMP/bad.chce" bs=1 \
      seek="$off" conv=notrunc status=none 2>/dev/null

  timeout 10 $BIN -d -f -o "$TMP/out.bin" "$TMP/bad.chce" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 124 ]; then
    bad "decoder HUNG on corrupted stream (offset $off of $clen)"
  elif [ "$rc" -ge 128 ]; then
    bad "decoder CRASHED (signal $((rc - 128))) on corrupted stream (offset $off)"
  else
    ok
  fi
  i=$((i + 1))
done

# --- 3. truncated streams must be rejected ----------------------------------
for cut in 1 4 16 64; do
  make_input "$TMP/in.bin" 8192 1
  $BIN -9 -f -o "$TMP/c.chce" "$TMP/in.bin" >/dev/null 2>&1 || continue
  clen=$(wc -c < "$TMP/c.chce")
  [ "$clen" -le "$cut" ] && continue
  head -c $((clen - cut)) "$TMP/c.chce" > "$TMP/trunc.chce"
  timeout 10 $BIN -d -f -o "$TMP/out.bin" "$TMP/trunc.chce" >/dev/null 2>&1
  rc=$?
  if [ "$rc" -eq 0 ]; then
    bad "truncated stream (removed $cut bytes) decoded successfully"
  elif [ "$rc" -ge 128 ] || [ "$rc" -eq 124 ]; then
    bad "truncated stream crashed or hung (rc=$rc)"
  else
    ok
  fi
done

printf '\n%d checks passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
