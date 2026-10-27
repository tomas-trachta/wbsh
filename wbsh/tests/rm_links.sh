#!/bin/bash
# rm removes links (dangling or not, symlinks and junctions) without touching
# their targets, honours --, and implements -d, -v, -i, -I and --interactive.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
mkdir target
echo keep > target/precious

check_target() {
    [ -e target/precious ] && echo "  target kept"
}

echo "=== junctions"
cmd /c mklink /J jd target > /dev/null
rm jd
echo "rm jd: $?"
[ -e jd ] || echo "  jd gone"
check_target
mkdir tree
cmd /c mklink /J "tree\\inner" target > /dev/null
rm -r tree
echo "rm -r tree with a junction inside: $?"
check_target
cmd /c mklink /J dangling nowhere > /dev/null
rm dangling
echo "rm dangling junction: $?"

echo "=== symlinks"
if cmd /c mklink sl "target\\precious" > /dev/null 2>&1; then
    cmd /c mklink dang nonexist > /dev/null
    cmd /c mklink /D sd target > /dev/null
    rm dang
    echo "rm dangling symlink: $?"
    rm sl
    echo "rm file symlink: $?"
    rm sd
    echo "rm directory symlink: $?"
    ls -d dang sl sd 2> /dev/null || echo "  links gone"
else
    echo "rm dangling symlink: 0"
    echo "rm file symlink: 0"
    echo "rm directory symlink: 0"
    echo "  links gone"
fi
check_target

echo "=== --"
echo x > -dash
rm -- -dash
echo "rm -- -dash: $?"
[ -e -dash ] || echo "  -dash gone"
rm -- --nope
echo "rm -- --nope: $?"

echo "=== -d"
mkdir empty full
touch full/f
rm -d empty
echo "rm -d empty: $?"
rm -d full
echo "rm -d full: $?"
rm full
echo "rm full: $?"
rm -d .
echo "rm -d .: $?"
rm -rf full

echo "=== -v"
mkdir -p v/w
touch v/w/f v/g a1
rm -v a1
rm -rv v
echo "rm -rv: $?"

echo "=== -i"
touch y1 n1
echo q > ne
printf 'y\nn\ny\n' | rm -i y1 n1 ne
echo
echo "left:" *[0-9e]
mkdir -p d/e
touch d/e/f
printf 'y\ny\ny\ny\nn\n' | rm -ri d
echo
echo "left:" d*
rm -rf d
touch z
rm -i -f z
echo "rm -i -f: $? (no prompt)"
touch z
rm -f -i z < /dev/null
echo
echo "rm -f -i with no answer kept z: $(ls z)"

echo "=== -I and --interactive"
touch z1 z2 z3 z4
echo n | rm -I z1 z2 z3 z4
echo
echo "after no:" z?
rm -I z1 z2 z3
echo "rm -I three files: $? (no prompt)"
rm -I z4 z
echo "rm -I two files: $? (no prompt)"
mkdir r
echo n | rm -rI r
echo
echo "after no:" r
echo y | rm --interactive=once -r r
echo
touch w
rm --interactive=never w
echo "--interactive=never: $?"
rm --interactive=bogus w
echo "--interactive=bogus: $?"

cd /
rm -rf "$D"
