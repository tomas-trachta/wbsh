#!/bin/bash
# awk's length, substr, index and match count UTF-8 characters, as gawk
# does in a UTF-8 locale and as wc -m does; an invalid byte is one
# character.

echo "=== length"
echo 'žluťoučký kůň' | awk '{ print length($0), length() }'
awk 'BEGIN { print length("čřž"), length("") }'
printf 'a\377b\n' | awk '{ print length() }'

echo "=== substr"
echo 'žluťoučký' | awk '{ print substr($0, 2, 3) "|" substr($0, 8) "|" substr($0, 0, 2) "|" substr($0, 20) "|" }'

echo "=== index"
echo 'kůň a pes' | awk '{ print index($0, "ň"), index($0, "pes"), index($0, "x") }'

echo "=== match"
echo 'kůň-pes' | awk '{ print match($0, /ň-p/), RSTART, RLENGTH }'
echo 'abc' | awk '{ print match($0, /z/), RSTART, RLENGTH }'

echo "=== ASCII is unchanged"
echo 'hello world' | awk '{ print length(), substr($0, 7), index($0, "o"), match($0, /wor/), RLENGTH }'
