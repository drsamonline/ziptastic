#!/usr/bin/env bash
# test_cli.sh — Phase 0 CLI integration tests.
#
# Exercises the exact smoke test from AGENT_PROMPT.md (echo hi | ziptastic |
# ziptastic -d) plus the file, pipe, overwrite and error paths.
set -u

BIN="${ZIPT_BIN:?ZIPT_BIN must point at the ziptastic binary}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
pass=0

ok()   { pass=$((pass + 1)); printf '[ ok ] %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '[FAIL] %s\n' "$1"; }

expect_ok()   { if [ "$1" -eq 0 ]; then ok "$2"; else bad "$2 (exit $1)"; fi; }
expect_fail() { if [ "$1" -ne 0 ]; then ok "$2"; else bad "$2 (expected non-zero exit)"; fi; }

# ---------------------------------------------------------------- version/help
out="$("$BIN" --version)"; rc=$?
expect_ok $rc "--version exits 0"
case "$out" in
  *ziptastic*) ok "--version prints the tool name" ;;
  *) bad "--version output: $out" ;;
esac

"$BIN" --help >"$TMP/help.txt" 2>&1; rc=$?
expect_ok $rc "--help exits 0"
grep -q "usage: ziptastic" "$TMP/help.txt" && ok "--help prints usage" \
  || bad "--help output missing usage line"

"$BIN" --bogus-flag >/dev/null 2>&1
expect_fail $? "unknown option is rejected"

# ------------------------------------------------------------- stdin/stdout
printf 'hi' | "$BIN" > "$TMP/hi.chce" 2>"$TMP/err.txt"; rc=$?
expect_ok $rc "compress stdin -> stdout"
printf 'hi' | "$BIN" | "$BIN" -d > "$TMP/hi.out" 2>>"$TMP/err.txt"; rc=$?
expect_ok $rc "echo hi | ziptastic | ziptastic -d"
[ "$(cat "$TMP/hi.out")" = "hi" ] && ok "piped round-trip restores 'hi'" \
  || bad "piped round-trip content: $(cat "$TMP/hi.out")"

# --------------------------------------------------------------- empty input
: | "$BIN" > "$TMP/empty.chce"; rc=$?
expect_ok $rc "compress empty input"
: | "$BIN" | "$BIN" -d > "$TMP/empty.out"; rc=$?
expect_ok $rc "empty round-trip"
[ ! -s "$TMP/empty.out" ] && ok "empty round-trip is empty" || bad "empty round-trip produced bytes"

# --------------------------------------------------------------- binary file
head -c 1048576 /dev/urandom > "$TMP/rand.bin"
"$BIN" -f -o "$TMP/rand.chce" "$TMP/rand.bin"; rc=$?
expect_ok $rc "compress 1 MiB random file"
"$BIN" -d -f -o "$TMP/rand.out" "$TMP/rand.chce"; rc=$?
expect_ok $rc "decompress 1 MiB random file"
cmp -s "$TMP/rand.bin" "$TMP/rand.out" && ok "1 MiB round-trip is bit-exact" \
  || bad "1 MiB round-trip differs"

# positional output argument form
"$BIN" -f "$TMP/rand.bin" "$TMP/rand2.chce"; rc=$?
expect_ok $rc "positional input/output form"
cmp -s "$TMP/rand.chce" "$TMP/rand2.chce" && ok "output is deterministic" \
  || bad "output differs between runs"

# ------------------------------------------------------------ overwrite guard
"$BIN" -o "$TMP/rand.chce" "$TMP/rand.bin" >/dev/null 2>&1
expect_fail $? "existing output is not overwritten without -f"

# --------------------------------------------------------------- error paths
printf 'XXXXnot-a-chce-stream' > "$TMP/bad.chce"
"$BIN" -d "$TMP/bad.chce" >/dev/null 2>"$TMP/bad.err"; rc=$?
expect_fail $rc "bad magic is rejected"
grep -qi "magic" "$TMP/bad.err" && ok "bad magic error names the problem" \
  || bad "bad magic error message: $(cat "$TMP/bad.err")"

head -c 6 "$TMP/rand.chce" > "$TMP/trunc.chce"
"$BIN" -d "$TMP/trunc.chce" >/dev/null 2>&1
expect_fail $? "truncated stream is rejected"

# block size option
"$BIN" -b 1 -f -o "$TMP/bs1.chce" "$TMP/rand.bin"; rc=$?
expect_ok $rc "-b 1 (1 KiB blocks) compresses"
"$BIN" -d -f -o "$TMP/bs1.out" "$TMP/bs1.chce"
cmp -s "$TMP/rand.bin" "$TMP/bs1.out" && ok "-b 1 round-trip is bit-exact" \
  || bad "-b 1 round-trip differs"

# bench command
"$BIN" bench "$TMP/rand.bin" > "$TMP/bench.txt" 2>&1; rc=$?
expect_ok $rc "bench runs"
grep -q "TOTAL" "$TMP/bench.txt" && ok "bench prints a total row" \
  || bad "bench output: $(cat "$TMP/bench.txt")"

# ------------------------------------------------------- XML numeric transform
# A bare spreadsheet sheet, the shape an .xlsx inflates to. The transform must
# engage (the layers byte says so) and the round trip must be bit-exact.
{
  printf '<?xml version="1.0"?><worksheet><sheetData>'
  i=1; a=1000
  while [ $i -le 400 ]; do
    a=$((a + 3))
    printf '<row r="%d"><c r="A%d"><v>%d</v></c><c r="B%d"><v>%d</v></c></row>' \
      "$i" "$i" "$a" "$i" "$((a * 7))"
    i=$((i + 1))
  done
  printf '</sheetData></worksheet>'
} > "$TMP/sheet.xml"

"$BIN" -9 -f -o "$TMP/sheet.chce" "$TMP/sheet.xml"; rc=$?
expect_ok $rc "spreadsheet XML compresses"
layers=$(od -An -tu1 -j6 -N1 "$TMP/sheet.chce" | tr -d ' ')
[ "$layers" = "4" ] && ok "XML numeric layer bit is set (layers=0x04)" \
  || bad "XML numeric layer bit not set (layers=$layers)"
"$BIN" -d -f -o "$TMP/sheet.out" "$TMP/sheet.chce"
cmp -s "$TMP/sheet.xml" "$TMP/sheet.out" && ok "spreadsheet XML round-trip is bit-exact" \
  || bad "spreadsheet XML round-trip differs"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
