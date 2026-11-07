#!/usr/bin/env bash
# silesia.sh — the one benchmark row anyone else can check.
#
# FINDINGS.md 6a quotes ziptastic against an `xz -9e` equivalent on the Silesia corpus. This
# regenerates it: fetch the corpus, compress every file with both, print the table.
#
#   scripts/silesia.sh [workdir]
#
# The corpus is fetched from the GitHub mirror because the canonical host is not reachable from
# every sandbox; the twelve files and their sizes are checked before anything is measured.
set -u

BIN="${ZIPT_BIN:?set ZIPT_BIN to the ziptastic binary}"
XZC="${XZC_BIN:-}"            # optional: an xz -9e equivalent, see tests/benchmark
WORK="${1:-/tmp/silesia}"
CORPUS="https://github.com/MiloszKrajewski/SilesiaCorpus.git"

# The twelve files and their exact sizes. A corpus that silently differs is a table that lies.
expected="dickens:10192446 mozilla:51220480 mr:9970564 nci:33553445 ooffice:6152192 osdb:10085684 reymont:6627202 samba:21606400 sao:7251944 webster:41458703 x-ray:8474240 xml:5345280"

if [ ! -d "$WORK/SilesiaCorpus" ]; then
  mkdir -p "$WORK" || exit 1
  git clone --depth 1 -q "$CORPUS" "$WORK/SilesiaCorpus" || { echo "cannot fetch the corpus"; exit 1; }
fi
cd "$WORK/SilesiaCorpus" || exit 1
mkdir -p corpus
for z in *.zip; do unzip -qo "$z" -d corpus; done

bad=0
for pair in $expected; do
  name="${pair%%:*}"; size="${pair##*:}"
  actual=$(stat -c%s "corpus/$name" 2>/dev/null || echo 0)
  [ "$actual" = "$size" ] || { echo "corpus differs: $name is $actual, expected $size"; bad=1; }
done
[ "$bad" -eq 0 ] || exit 1

# Compression writes hundreds of megabytes. A full /tmp once produced a truncated stream that read
# as a 47% regression, so this refuses to start without room rather than repeating that.
avail=$(df -Pk "$WORK" | awk 'NR==2 {print $4}')
[ "$avail" -gt 500000 ] || { echo "need 500 MB free in $WORK, have $((avail/1024)) MB"; exit 1; }

printf "%-10s %12s %12s" file input ziptastic
[ -n "$XZC" ] && printf " %12s %7s" xz-9e gain
echo
tz=0; tin=0; tx=0
# SILESIA_ONLY=file1,file2 restricts the run. The full corpus is about 15 minutes of compression;
# this exists so the script can be smoke-tested in one call rather than trusted.
only="${SILESIA_ONLY:-}"
for pair in $expected; do
  name="${pair%%:*}"; size="${pair##*:}"
  if [ -n "$only" ]; then
    case ",$only," in *",$name,"*) ;; *) continue ;; esac
  fi
  "$BIN" -9 -f -o "$WORK/$name.dr" "corpus/$name" 2>/dev/null
  z=$(stat -c%s "$WORK/$name.dr")
  tz=$((tz+z)); tin=$((tin+size))
  if [ -n "$XZC" ]; then
    "$XZC" "corpus/$name" "$WORK/$name.xz"
    x=$(stat -c%s "$WORK/$name.xz")
    # A truncated stream is a wrong number. Decode it back and compare before believing it.
    if ! python3 - "$WORK/$name.xz" "corpus/$name" <<'PYEOF'
import lzma, sys
blob = open(sys.argv[1], "rb").read()
raw = open(sys.argv[2], "rb").read()
sys.exit(0 if lzma.decompress(blob) == raw else 1)
PYEOF
    then echo "  $name: xz stream does not decode back, refusing to report it"; exit 1; fi
    tx=$((tx+x))
    printf "%-10s %12d %12d %12d %6.1f%%\n" "$name" "$size" "$z" "$x" \
      "$(python3 -c "print(100*($x-$z)/$x)")"
  else
    printf "%-10s %12d %12d\n" "$name" "$size" "$z"
  fi
done
printf "%-10s %12d %12d" total "$tin" "$tz"
if [ -n "$XZC" ]; then
  printf " %12d %6.1f%%\n" "$tx" "$(python3 -c "print(100*($tx-$tz)/$tx)")"
  python3 -c "print('\nziptastic %.2f%%   xz -9e %.2f%%' % (100*$tz/$tin, 100*$tx/$tin))"
else
  echo
  python3 -c "print('\nziptastic %.2f%%' % (100*$tz/$tin))"
fi
