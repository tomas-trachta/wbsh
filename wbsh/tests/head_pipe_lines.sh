#!/bin/bash
# head -n and grep answer from a pipe as soon as the lines they need have
# arrived, instead of waiting for a 64 KiB chunk or the end of the input.
# Each reader runs in a group whose $SECONDS starts with the producer.

verdict() {
    if [ "$SECONDS" -le 1 ]; then echo "answered at once"; else echo "waited ${SECONDS}s"; fi
}

echo "=== head -n stops reading after its lines"
(echo a; sleep 3; echo b) | { head -n1; verdict; }
(echo a; echo b; sleep 3; echo c) | { head -n 2; verdict; }

echo "=== head -n with a negative count and tail +N still see everything"
(echo a; echo b; echo c) | head -n -1
(echo a; echo b; echo c) | tail -n +2

echo "=== grep -m and grep -q stop at the first match"
(echo miss; echo hit; sleep 3; echo late) | { grep -m1 hit; verdict; }
(echo hit; sleep 3) | { grep -q hit && echo found; verdict; }

echo "=== files are still read in chunks"
D=$(mktemp -d) || exit 1
printf 'one\ntwo\nthree\n' > "$D/f.txt"
head -n 2 "$D/f.txt"
tail -n 1 "$D/f.txt"
grep -c o "$D/f.txt"
rm -rf "$D"
