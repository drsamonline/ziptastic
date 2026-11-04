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

# ---------------------------------------------------------- default names (.dr)
# `c` with no output path writes <input>.dr, `d` strips it, a renamed stream still decodes and
# takes a .out name, and a .dr that is not a CHCE stream is refused. Detection is by magic, so
# the extension is a convention and never a gate.
head -c 20000 /dev/urandom > "$TMP/dr.bin"
"$BIN" -9 "$TMP/dr.bin" 2>/dev/null; rc=$?
[ $rc -eq 0 ] && [ -f "$TMP/dr.bin.dr" ] && ok "c with no -o writes <input>.dr" || fail "c wrote no .dr"

rm -f "$TMP/dr.bin"
"$BIN" d "$TMP/dr.bin.dr" 2>/dev/null; rc=$?
[ $rc -eq 0 ] && [ -f "$TMP/dr.bin" ] && ok "d strips the .dr extension" || fail "d did not restore the name"

cp "$TMP/dr.bin.dr" "$TMP/renamed.bin"
"$BIN" d "$TMP/renamed.bin" 2>/dev/null; rc=$?
if [ $rc -eq 0 ] && cmp -s "$TMP/renamed.bin.out" "$TMP/dr.bin"; then
  ok "a renamed stream decodes to <name>.out"
else
  fail "a renamed stream did not decode by magic"
fi

head -c 5000 /dev/urandom > "$TMP/fake.dr"
"$BIN" d "$TMP/fake.dr" 2>/dev/null; rc=$?
[ $rc -ne 0 ] && ok "a .dr that is not CHCE is refused" || fail "a fake .dr was accepted"

# --------------------------------------------------------------- split output
# The parts are pieces of one stream, so they must all be present; any part can be
# handed to the decoder and it joins the siblings itself.
"$BIN" -9 --split-size 4K -f -o "$TMP/split.zt" "$TMP/rand.bin" 2>/dev/null; rc=$?
expect_ok $rc "--split-size writes parts"
n=$(ls "$TMP"/split.zt.* 2>/dev/null | wc -l)
[ "$n" -ge 2 ] && ok "--split-size produced $n parts" || bad "--split-size produced $n parts"
"$BIN" -d -f -o "$TMP/split.out" "$TMP/split.zt.001"
cmp -s "$TMP/rand.bin" "$TMP/split.out" && ok "a part decodes by joining its siblings" \
  || bad "split round-trip differs"
last=$(ls "$TMP"/split.zt.* | tail -1)
"$BIN" -d -f -o "$TMP/split2.out" "$last"
cmp -s "$TMP/rand.bin" "$TMP/split2.out" && ok "the last part decodes too" \
  || bad "decoding from the last part differs"
mv "$TMP/split.zt.002" "$TMP/held.002"
"$BIN" -d -f -o "$TMP/split3.out" "$TMP/split.zt.001" >/dev/null 2>&1
expect_fail $? "a missing part is refused"
mv "$TMP/held.002" "$TMP/split.zt.002"

# ------------------------------------------------------------ base64 transform
# Base64 of random bytes, wrapped at 76 columns — the shape a MIME attachment or a
# JSON blob takes. The transform must engage (the layers byte says so) and the
# round trip must be bit-exact.
head -c 3000 /dev/urandom | base64 > "$TMP/blob.txt"
"$BIN" -9 -f -o "$TMP/blob.chce" "$TMP/blob.txt"; rc=$?
expect_ok $rc "base64 blob compresses"
blayers=$(od -An -tu1 -j6 -N1 "$TMP/blob.chce" | tr -d ' ')
[ "$blayers" = "16" ] && ok "base64 layer bit is set (layers=0x10)" \
  || bad "base64 layer bit not set (layers=$blayers)"
"$BIN" -d -f -o "$TMP/blob.out" "$TMP/blob.chce"
cmp -s "$TMP/blob.txt" "$TMP/blob.out" && ok "base64 round-trip is bit-exact" \
  || bad "base64 round-trip differs"

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

# --------------------------------------------------- block offset table (v8)
# Self-contained blocks, so one can be decoded without the others, and no transform, which
# is the other thing --block requires.
#
# The fixture is `seq` rather than /dev/urandom, and every gate that could fire is turned
# off by hand. Both matter: a random fixture is a different file on every machine, and on
# the CI runner a 3 MiB blob of it happened to make the BCJ gate fire, so `--block` refused
# a stream that had been addressable here. A test whose subject is the offset table should
# not also be a test of which transforms win on bytes nobody can reproduce.
seq 1 700000 > "$TMP/blocks.bin"
"$BIN" -f --parallel --no-lzma2 --no-bcj --no-base64 --no-numeric --no-precomp \
       -b 1024 -o "$TMP/blocks.chce" "$TMP/blocks.bin"; rc=$?
expect_ok $rc "compress a text fixture as self-contained blocks"

info="$("$BIN" info "$TMP/blocks.chce")"; rc=$?
expect_ok $rc "info exits 0"
printf '%s\n' "$info" | grep -q "verified against the manifest digest" \
  && ok "info reports the block table as verified" \
  || bad "info did not report a verified block table"
printf '%s\n' "$info" | grep -Eq '^blocks +5$' \
  && ok "info prints the block count" || bad "info block count line missing"
printf '%s\n' "$info" | grep -q "each block decodes on its own" \
  && ok "info says the blocks are addressable" || bad "info did not report block access"

"$BIN" d --block 1 -f -o "$TMP/blk1.out" "$TMP/blocks.chce"; rc=$?
expect_ok $rc "decode a single block from its offset"
dd if="$TMP/blocks.bin" bs=1048576 skip=1 count=1 2>/dev/null > "$TMP/blk1.want"
cmp -s "$TMP/blk1.want" "$TMP/blk1.out" \
  && ok "the block decoded on its own is the right slice" || bad "single-block decode differs"

"$BIN" d --from-block 2 -f -o "$TMP/tail.out" "$TMP/blocks.chce"; rc=$?
expect_ok $rc "decode from block 2 to the end"
dd if="$TMP/blocks.bin" bs=1048576 skip=2 2>/dev/null > "$TMP/tail.want"
cmp -s "$TMP/tail.want" "$TMP/tail.out" \
  && ok "the resumed decode is the right tail" || bad "resumed decode differs"

# An archive-mode stream has an offset table too, but its blocks may reference each
# other: the CLI must refuse rather than return bytes that look right.
"$BIN" -f --no-lzma2 --no-bcj --no-base64 --no-numeric --no-precomp \
       -b 1024 -o "$TMP/arch.chce" "$TMP/blocks.bin"; rc=$?
expect_ok $rc "compress the same data in archive mode"
"$BIN" d --block 1 -f -o "$TMP/arch.out" "$TMP/arch.chce" 2>"$TMP/arch.err"; rc=$?
expect_fail $rc "archive-mode --block is refused"
grep -q "archive mode" "$TMP/arch.err" \
  && ok "the refusal names the reason" || bad "refusal message: $(cat "$TMP/arch.err")"

"$BIN" d --block 99 -f -o "$TMP/none.out" "$TMP/blocks.chce" >/dev/null 2>&1
expect_fail $? "a block index past the end is refused"

# A v7 stream is not a v8 stream and must not be read as one.
head -c 200 /dev/urandom > "$TMP/v7.bin"
printf 'CHCE\x07' | dd of="$TMP/v7.bin" conv=notrunc 2>/dev/null
"$BIN" d -f -o "$TMP/v7.out" "$TMP/v7.bin" 2>/dev/null
expect_fail $? "a stream claiming format v7 is refused"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
