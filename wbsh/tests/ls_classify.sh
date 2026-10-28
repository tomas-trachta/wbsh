#!/bin/bash
# ls prints bare names unless -F / -p / --file-type / --indicator-style ask
# for type suffixes. Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

mkdir a
touch run.exe script.bat plain.txt
cmd /c mklink /J j a > /dev/null

echo "=== plain"
ls | cat
echo "=== names round-trip through read"
ls | while read -r f; do [ -e "$f" ] && echo "ok $f"; done
echo "=== -F"
ls -F
echo "=== --classify=always"
ls --classify=always
echo "=== --classify=auto (not a tty)"
ls --classify=auto
echo "=== -p"
ls -p
echo "=== --file-type"
ls --file-type
echo "=== --indicator-style=classify then none"
ls --indicator-style=classify --indicator-style=none
echo "=== -l without and with -F"
ls -l | sed 's/^.* //'
ls -lF | sed 's/^.* //'
echo "=== -d on a directory"
ls -d a
ls -dF a
echo "=== errors"
ls --indicator-style=bogus
echo "bad style: $?"
ls -z
echo "bad option: $?"
ls -- -nope 2> /dev/null
echo "after --: $?"

cd /
rm -rf "$D"
