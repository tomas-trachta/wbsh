#!/bin/bash
# grep -r never follows links met during the walk (only operands); grep -R
# follows them and stops at a directory that is its own ancestor.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
mkdir -p jt/sub other
echo needle > jt/sub/f.txt
echo needle > other/g.txt
cmd /c mklink /J "jt\\sub\\loop" jt > /dev/null
cmd /c mklink /J "jt\\side" other > /dev/null

echo "=== -r skips junctions inside the tree"
grep -r needle jt
echo "rc=$?"

echo "=== -r follows a junction given as operand"
grep -r needle jt/side
echo "rc=$?"

echo "=== -R follows junctions and reports the loop"
grep -R needle jt 2>&1
echo "rc=$?"

echo "=== -R -s silences the loop warning"
grep -Rs needle jt 2>&1
echo "rc=$?"

echo "=== --dereference-recursive is -R"
grep --dereference-recursive -c needle jt/sub 2>&1

echo "=== -r without operand searches ."
cd jt
grep -r needle
echo "rc=$?"
cd ..

echo "=== -R loop through an operand junction"
grep -R needle jt/sub/loop/sub 2>&1

cd /
rm -rf "$D"
