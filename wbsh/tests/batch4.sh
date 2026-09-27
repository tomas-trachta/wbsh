#!/bin/bash
echo "=== sed"
printf 'one apple\ntwo apples\n' | sed 's/apple/orange/'
echo --
printf 'one apple\nan apple a day\n' | sed 's/apple/orange/g'
echo --
printf 'aaa\nbbb\nccc\n' | sed -n -e 's/b/X/'   # -n suppresses output
echo "(quiet ok)"
echo --
printf 'foo123bar\n' | sed 's/\([a-z]*\)\([0-9]*\)\([a-z]*\)/\3-\2-\1/'

echo "=== stat"
stat tests/batch4.sh | head -3

echo "=== chmod / readonly attribute"
echo hi > /tmp/wbsh-cmod.t
chmod -w /tmp/wbsh-cmod.t
echo NOPE > /tmp/wbsh-cmod.t 2>&1 || echo "(write blocked as expected)"
chmod +w /tmp/wbsh-cmod.t
echo OK > /tmp/wbsh-cmod.t
cat /tmp/wbsh-cmod.t
rm -f /tmp/wbsh-cmod.t

echo "=== ln (hard link)"
echo content > /tmp/wbsh-ln-src
ln /tmp/wbsh-ln-src /tmp/wbsh-ln-hard
cat /tmp/wbsh-ln-hard
rm -f /tmp/wbsh-ln-src /tmp/wbsh-ln-hard

echo "=== readonly / declare"
declare -r RX=42
RX=99 2>&1 || true
echo "RX=$RX"
declare -p RX 2>/dev/null | head -1

echo "=== getopts"
showopts() {
  local OPTIND=1
  while getopts "ab:c" opt "$@"; do
    case $opt in
      a) echo "got a";;
      b) echo "got b with arg=$OPTARG";;
      c) echo "got c";;
      ?) echo "unknown";;
    esac
  done
}
showopts -a -b VALUE -c

echo "=== trap (EXIT)"
( trap 'echo "(exit trap fired)"' EXIT
  echo "before exit" )
echo "after subshell"

echo "=== done"
