#!/bin/bash
# Smoke test for path translation.

echo "=== 1. cd / pwd"
pwd
cd /c/Users/trach/Desktop
pwd
cd /c
pwd
cd -

echo "=== 2. /tmp"
echo "hello tmp" > /tmp/wbsh-paths.t
cat /tmp/wbsh-paths.t
rm /tmp/wbsh-paths.t

echo "=== 3. /dev/null"
echo nope >/dev/null
cat /dev/null && echo "cat /dev/null ok"

echo "=== 4. test on POSIX path"
if [ -d /c/Users ]; then echo "/c/Users is a dir"; fi
if [ -f /c/Windows/win.ini ]; then echo "win.ini exists"; fi
if [ ! -e /c/no-such-xyz ]; then echo "absent ok"; fi

echo "=== 5. external takes POSIX path arg"
wc -l /c/Users/trach/Desktop/software_projects/wbsh/FEATURES.md

echo "=== 6. \$PATH and \$HOME shape"
echo "\$HOME = $HOME"
echo "\$PWD  = $PWD"
case "$PATH" in
  /*) echo "PATH starts with /, looks POSIX" ;;
  *)  echo "PATH not POSIX: $PATH" | head -c 80 ;;
esac

echo "=== 7. PATH inheritance to children"
# Child inherits Win32 PATH; grep uses it to resolve subprograms.
which grep 2>/dev/null || command -v grep

echo "=== done"
