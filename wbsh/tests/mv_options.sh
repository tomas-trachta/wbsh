#!/bin/bash
# mv refuses to move a directory into itself, never falls back to copying
# for same-volume failures, and implements -f -i -n -u -v -t -T and --.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

echo "=== into itself"
mkdir m
echo 1 > m/a
mv m m/sub
echo "mv m m/sub: $?"
mkdir m/sub
mv m m/sub
echo "mv m m/sub (existing): $?"
mv m ./m/sub/deeper
echo "mv m ./m/sub/deeper: $?"
echo "m still:" m/*
mv m m
echo "mv m m: $?"
mv m .
echo "mv m .: $?"

echo "=== plain moves"
echo a > a
mv a b
echo "mv a b: $? -> $(cat b)"
mkdir dir
mv b dir
echo "into dir: $? ->" dir/*
echo c > c
echo e > e
mv c e dir
echo "several into dir: $? ->" dir/*
mv dir/c dir/e .
echo s > s
mv s S
echo "case-only rename: $? ->" [sS]
mkdir old
echo x > old/x
mv old new
echo "rename dir: $? ->" new/*

echo "=== -n -i -f -u"
echo src > src
echo dst > dst
mv -n src dst
echo "mv -n: $? src=$(cat src) dst=$(cat dst)"
echo "prompt: $(echo n | mv -i src dst 2>&1)"
echo "mv -i no: $? src=$(cat src) dst=$(cat dst)"
echo "prompt: $(echo y | mv -i src dst 2>&1)"
echo "mv -i yes: $? dst=$(cat dst)"
echo src > src
mv -i -f src dst
echo "mv -i -f: $? dst=$(cat dst) (no prompt)"
echo src > src
mv -f -n src dst
echo "mv -f -n: $? kept src: $(cat src)"
sleep 0.1
echo newer > dst
mv -u src dst
echo "mv -u older: $? src=$(cat src) dst=$(cat dst)"
sleep 0.1
echo newest > src
mv -u src dst
echo "mv -u newer: $? dst=$(cat dst)"
echo ro > ro
echo new > replacer
attrib +r ro
mv replacer ro
echo "onto read-only: $? -> $(cat ro)"

echo "=== -v -t -T --"
echo v > v
mkdir t
mv -v v t
mv -v -t . t/v
mv -T t tdir
echo "mv -T: $? ->" tdir
mkdir full
touch full/x
mv -T tdir full
echo "mv -T onto non-empty dir: $?"
mkdir empty
mv -v -T tdir empty
echo "mv -T onto empty dir: $?"
echo d > -dash
mv -- -dash dash
echo "mv --: $? ->" dash

echo "=== errors"
mv
echo "no operands: $?"
mv a
echo "one operand: $?"
mv nope x
echo "missing source: $?"
mv dash v nofile
echo "several into a non-directory: $?"
mkdir sub
mv sub dash
echo "dir onto file: $?"
mkdir -p g/v
mv v g
echo "file onto dir: $?"
mv -T dash v y
echo "mv -T extra operand: $?"
mv -t nodir dash
echo "mv -t missing dir: $?"
mv -t g -T dash
echo "mv -t with -T: $?"
mv --bogus a b
echo "unknown option: $?"

cd /
rm -rf "$D"
