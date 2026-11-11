#!/bin/bash
# A `[` with no unquoted `]` after it is not a glob, so nullglob keeps it:
# `[ x = y ]` stays a test instead of running `x` (#291).

mkdir -p _run_tmp/ngb && cd _run_tmp/ngb && touch a b ab
shopt -s nullglob

echo "=== the [ command"
[ x = y ]; echo "x = y: rc=$?"
[ 1 -eq 1 ]; echo "1 -eq 1: rc=$?"
[ -d . ]; echo "-d .: rc=$?"

echo "=== words with an unmatched [ stay literal"
echo [ [a a] "(end)"
echo "quoted ]:" [a"]"

echo "=== bracket expressions still glob"
echo "[ab]:" [ab]
echo "[ab]*:" [ab]*
echo "[z]:" [z] "(end)"

shopt -u nullglob
cd ../.. && rm -rf _run_tmp/ngb
