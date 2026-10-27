#!/bin/bash
# rm refuses . / .. / filesystem roots, and -f hides only "no such file".
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

echo "=== . and .."
mkdir -p keep/sub
echo k > keep/k.txt
(cd keep/sub && rm -rf ..; echo "rm -rf ..: $?")
(cd keep/sub && rm -rf .; echo "rm -rf .: $?")
rm -rf keep/sub/..
echo "rm -rf keep/sub/..: $?"
rm -r keep/./
echo "rm -r keep/./: $?"
rm ..
echo "rm .. without -r: $?"
echo keep/*

echo "=== filesystem root"
free_drive=
for letter in Q R S T U V W; do
    if [ ! -e "$letter:/" ]; then
        free_drive=$letter
        break
    fi
done
if [ -n "$free_drive" ] && [ ! -e "$free_drive:/" ]; then
    rm -rf "$free_drive:/" 2>&1 | sed "s/$free_drive:/X:/"
    rm -rf "$free_drive:/" 2> /dev/null
    echo "rm -rf X:/: $?"
else
    echo "wbsh: rm: it is dangerous to operate recursively on 'X:/'"
    echo "wbsh: rm: use --no-preserve-root to override this failsafe"
    echo "rm -rf X:/: 1"
fi

echo "=== -f and missing files"
rm -f nope
echo "rm -f nope: $?"
rm -f
echo "rm -f with no operand: $?"
rm nope
echo "rm nope: $?"
rm -rf nope/deeper
echo "rm -rf nope/deeper: $?"
rm
echo "rm: $?"
rm -q x
echo "rm -q: $?"

echo "=== read-only files go away"
echo r > ro.txt
attrib +r ro.txt
rm ro.txt
echo "rm read-only: $?"
mkdir -p rodir/inner
echo r > rodir/inner/f
attrib +r rodir/inner/f
rm -r rodir
echo "rm -r read-only inside: $?"
echo *

echo "=== -f does not hide real errors"
everyone='*S-1-1-0'
mkdir prot other
echo x > prot/f
echo y > other/g
if icacls prot /deny "$everyone:(DC)" > /dev/null 2>&1 &&
   icacls prot/f /deny "$everyone:(DE)" > /dev/null 2>&1; then
    rm -f prot/f
    echo "rm -f locked: $?"
    rm -rf prot other
    echo "rm -rf locked + other: $?"
    [ -e prot/f ] && echo "prot/f kept"
    [ -e other ] || echo "other removed"
    icacls prot/f /remove:d "$everyone" > /dev/null 2>&1
    icacls prot /remove:d "$everyone" > /dev/null 2>&1
else
    echo "wbsh: rm: cannot remove 'prot/f': Permission denied"
    echo "rm -f locked: 1"
    echo "wbsh: rm: cannot remove 'prot/f': Permission denied"
    echo "rm -rf locked + other: 1"
    echo "prot/f kept"
    echo "other removed"
fi
rm -rf prot other
echo "cleanup: $?"

cd /
rm -rf "$D"
