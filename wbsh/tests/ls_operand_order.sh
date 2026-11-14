#!/bin/bash
# ls lists the operands that are not directories first, as one sorted
# block, then each directory under its own header, as GNU ls does.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

mkdir d2 d1
touch z.o a.o d1/x

ls d2 z.o d1 a.o
echo ---
ls z.o d1
echo ---
ls d1
echo ---
ls -d d2 z.o d1

cd /
rm -rf "$D"
