#!/bin/bash
# cp omits directories without -r and implements -a -p -n -i -u -v -L -P -t -T.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

same_time() {
    [[ ! "$1" -nt "$2" && ! "$1" -ot "$2" ]]
}

echo "=== directories need -r"
mkdir a
echo x > a/f.txt
cp a cpa
echo "cp a cpa: $?"
[ -e cpa ] || echo "  cpa not created"
echo y > y.txt
cp a y.txt out.d 2> /dev/null
mkdir out
cp a y.txt out
echo "cp a y.txt out: $? ->" out/*
cp -r a cpa
echo "cp -r a cpa: $? ->" cpa/*
cp -R a cpa
echo "cp -R into existing: $? ->" cpa/*
cp -rT a cpb
cp -rT a cpb
echo "cp -rT twice: $? ->" cpb/*
cp -r a a/sub
echo "cp -r a a/sub: $?"
cp y.txt ./y.txt
echo "same file: $?"
cp -r a y.txt
echo "dir onto file: $?"
mkdir -p g/y.txt
cp y.txt g
echo "file onto dir: $?"

echo "=== timestamps: -p, -a, default"
echo old > old.txt
sleep 1.1
cp old.txt plain.txt
cp -p old.txt kept.txt
cp -a a arch
cp --preserve=timestamps old.txt kept2.txt
cp -p --no-preserve=all old.txt fresh.txt
[[ plain.txt -nt old.txt ]] && echo "  plain copy gets a new time"
same_time kept.txt old.txt && echo "  -p keeps the time"
same_time arch/f.txt a/f.txt && echo "  -a keeps the time"
same_time kept2.txt old.txt && echo "  --preserve=timestamps keeps it"
[[ fresh.txt -nt old.txt ]] && echo "  --no-preserve=all drops it"
cp --preserve=bogus old.txt x 2>&1
echo "--preserve=bogus: $?"

echo "=== -n -i -f -u -v"
echo src > src
echo dst > dst
cp -n src dst
echo "cp -n: $? dst=$(cat dst)"
echo "prompt: $(echo n | cp -i src dst 2>&1)"
echo "cp -i no: dst=$(cat dst)"
echo "prompt: $(echo y | cp -i src dst 2>&1)"
echo "cp -i yes: dst=$(cat dst)"
echo newer > dst
cp -u old.txt dst
echo "cp -u older: $? dst=$(cat dst)"
sleep 0.1
echo newest > src
cp -u src dst
echo "cp -u newer: $? dst=$(cat dst)"
cp -u src brand-new
echo "cp -u missing dest: $? -> $(cat brand-new)"
echo ro > ro
attrib +r ro
cp src ro 2> /dev/null
echo "onto read-only: $? ro=$(cat ro)"
cp -f src ro
echo "cp -f onto read-only: $? ro=$(cat ro)"
cp -v src v1
cp -rv a av

echo "=== -t -T --"
mkdir t
cp -t t src dst
echo "cp -t: $? ->" t/*
cp -T src t 2>&1
echo "cp -T file onto dir: $?"
cp -T src dst extra
echo "cp -T extra operand: $?"
echo d > -dash
cp -- -dash dash
echo "cp --: $? -> $(cat dash)"

echo "=== links"
mkdir lt
echo in > lt/in
cmd /c mklink /J jd lt > /dev/null
mkdir tree
cmd /c mklink /J "tree\\j" lt > /dev/null
cp -r tree tree-P
[ -e tree-P/j/in ] && echo "  -r recreates the junction: $(cat tree-P/j/in)"
echo more > lt/more
[ -e tree-P/j/more ] && echo "  and it points at the same target"
cp -rL tree tree-L
echo "cp -rL: $? ->" tree-L/j/*
cp jd jd-copy 2>&1
echo "cp junction without -r: $?"
cp -r jd jd-r
echo "cp -r junction: $? ->" jd-r/*

echo "=== errors"
cp
echo "no operands: $?"
cp src
echo "one operand: $?"
cp nope x
echo "missing source: $?"
cp src dst nodir
echo "several into a non-directory: $?"
cp --bogus src dst
echo "unknown option: $?"

cd /
rm -rf "$D"
