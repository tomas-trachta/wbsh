#!/bin/bash
# Builtin output in bash's form: shopt -p and its table, umask -S, $_,
# DIRSTACK, caller, local -, and declare -f / type.

echo "=== 1. shopt"
shopt -s extglob
shopt extglob nullglob
echo "rc=$?"
shopt -p extglob nullglob
shopt -s | grep -c extglob
shopt -u -p | grep -c 'shopt -u nullglob'
shopt -u extglob

echo "=== 2. umask -S"
umask 022
umask -S
umask 0077
umask -S
umask 022

echo "=== 3. \$_ is the last argument of the previous command"
echo first arg last
echo "$_"
: one two
echo "$_"
f() { :; }
f x y
echo "$_"

echo "=== 4. pushd and popd keep DIRSTACK"
mkdir -p _run_tmp/ds/a/b
cd _run_tmp/ds
pushd a >/dev/null
pushd b >/dev/null
echo "${PWD##*/} depth=${#DIRSTACK[@]} top=${DIRSTACK[0]##*/} next=${DIRSTACK[1]##*/}"
popd >/dev/null
popd >/dev/null
echo "${PWD##*/} depth=${#DIRSTACK[@]}"
cd ../..

echo "=== 5. caller"
c() { caller; caller 0; caller 1; echo "rc=$?"; caller 0 | cat; }
d() { c; }
d
caller 0
echo "top rc=$?"

echo "=== 6. local - restores the set options"
lo() { local -; set -u; set -f; case $- in *u*f*|*f*u*) echo "inside: u and f" ;; esac; }
lo
case $- in *u*|*f*) echo "leaked: $-" ;; *) echo "restored" ;; esac

echo "=== 7. declare -f and type print bash's layout"
g() { echo hi; local x=1; }
declare -f g
h() {
	if [ -n "$1" ]; then echo yes; elif true; then :; else echo no; fi
	for i in a "b c"; do echo $i; done
	while read -r l; do echo "$l"; done < /dev/null
	case $1 in a|b) echo ab ;; *) ;; esac
	a && b || c | d
	( sub; shell ) > /dev/null 2>&1
	{ grp; }
	(( n = 1 + 2 ))
	bg & fg
}
declare -f h
k() ( echo in a subshell )
type k
