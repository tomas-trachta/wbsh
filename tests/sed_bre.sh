#!/bin/bash
# Exercises POSIX BRE features in sed (the default grammar) and ERE under
# `-E` / `-r`. Pinned because BRE `\(...\)` capture groups and `\1`
# back-references previously fell through wbsh's regex compile and were
# silently treated as literals.

echo "=== BRE: capture groups"
echo "foo123bar" | sed 's/\([a-z]*\)\([0-9]*\)\([a-z]*\)/\3-\2-\1/'

echo "=== BRE: leading-number capture"
printf '   42 hello world\n' | sed 's/^[ ]*\([0-9][0-9]*\).*$/\1/'

echo "=== BRE: anchors"
printf 'abc\nxabc\n' | sed 's/^abc$/MATCH/'

echo "=== BRE: zero-or-more"
# `*` is the only quantifier portable across POSIX BRE implementations.
# `\?` / `\+` are undefined in POSIX and intentionally unsupported here.
echo 'aaab' | sed 's/a*/X/'

echo "=== ERE: -E enables extended"
echo "foo123bar" | sed -E 's/([a-z]+)([0-9]+)([a-z]+)/\3-\2-\1/'
echo "axxxxb"   | sed -E 's/x+/-/'

echo "=== ERE: -r is an alias"
echo "axxxxb" | sed -r 's/x+/-/'

echo "=== mixed -e: chain a BRE and an ERE call"
# Each sed invocation has its own grammar. First strips the suffix in BRE,
# second collapses runs of x in ERE.
echo "fooxxxxbar.tmp" | sed 's/\.tmp$//' | sed -E 's/x+/-/'

echo "=== done"
