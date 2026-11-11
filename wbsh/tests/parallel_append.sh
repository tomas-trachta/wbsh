#!/bin/bash
# >> appends atomically: parallel writers lose no line and never mix two
# lines, as with O_APPEND on POSIX. Windows only does that for a handle
# opened with append access alone.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

for w in 1 2 3 4 5 6 7 8; do
    ( for i in $(seq 1 200); do echo "writer $w line $i" >> shared.txt; done ) &
done
wait
echo "lines: $(wc -l < shared.txt)"
echo "whole lines: $(grep -c '^writer [1-8] line [0-9]*$' shared.txt)"
echo "writer 5: $(grep -c '^writer 5 ' shared.txt)"

: > small.txt
echo one >> small.txt
echo two >> small.txt
cmd /c "echo three>>small.txt" < /dev/null
{ echo four; echo five; } >> small.txt
tr -d '\r' < small.txt

cd / && rm -rf "$D"
