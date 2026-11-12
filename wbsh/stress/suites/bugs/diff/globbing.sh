#!/bin/bash
# Pathname expansion, extglob, nullglob, dotglob and brace expansion.
mkdir -p gd && cd gd || exit 1
: > a.txt; : > b.txt; : > c.log; : > .hidden; mkdir -p sub; : > sub/d.txt
echo *.txt; echo ?.log; echo [ab].*; echo *; echo sub/*
echo nomatch*; shopt -s nullglob; echo "[" nomatch* "]"; shopt -u nullglob
shopt -s dotglob; echo *; shopt -u dotglob
shopt -s extglob
echo !(*.txt); echo @(a|c).*; echo +([a-c]).txt
shopt -u extglob
shopt -s globstar; echo **/*.txt; shopt -u globstar
echo {a,b}{1,2} {1..5} {05..10..2} {a..e} x{,y}z {z..w}
echo "{a,b}" '*.txt' \*.txt
f='*.txt'; echo $f "$f"
shopt -s extglob
[[ abc == a* ]] && echo m1; [[ abc == "a*" ]] || echo m2; [[ a.txt == *.@(txt|log) ]] && echo m3
