#!/bin/bash
# bash 5.3 additions: GLOBSORT, BASH_MONOSECONDS, BASH_TRAPSIG, read -E,
# source -p, printf's wide %ls / %lc, array_expand_once, patsub_replacement.

T=_bash53_tmp
rm -rf $T
mkdir -p $T

echo "=== GLOBSORT"
mkdir $T/g
printf 'aaaa' > $T/g/b; printf 'a' > $T/g/c; printf 'aa' > $T/g/a
mkdir $T/g/10 $T/g/9 $T/g/100
cd $T/g
for spec in '' name -name size -size numeric -numeric blocks +bogus - +; do
    GLOBSORT=$spec
    echo "[$spec] $(echo *)"
done
GLOBSORT=size; echo "size of [abc]: $(echo [abc])"
touch -d '2020-01-01 00:00:00' b; touch -d '2021-01-01 00:00:00' c; touch -d '2022-01-01 00:00:00' a
GLOBSORT=mtime; echo "mtime: $(echo [abc])"
GLOBSORT=-mtime; echo "-mtime: $(echo [abc])"
GLOBSORT=nosort; set -- *; echo "nosort keeps all $# names"
unset GLOBSORT
cd ../..

echo "=== BASH_MONOSECONDS"
m1=$BASH_MONOSECONDS
m2=$BASH_MONOSECONDS
(( m1 > 0 && m2 >= m1 )) && echo "monotonic and positive"
(( BASH_MONOSECONDS >= m1 )) && echo "usable in arithmetic"
BASH_MONOSECONDS=5
(( BASH_MONOSECONDS != 5 || m1 == 5 )) && echo "assignment ignored"

echo "=== BASH_TRAPSIG"
echo "outside: ${BASH_TRAPSIG-unset}"
trap 'echo "USR1: $BASH_TRAPSIG"' USR1
kill -USR1 $$
trap 'echo "ERR: $BASH_TRAPSIG"' ERR
false
trap - ERR
f() { :; }
set -o functrace
trap 'echo "RETURN: $BASH_TRAPSIG"' RETURN
f
trap - RETURN
set +o functrace
BASH_TRAPSIG=keep
kill -USR1 $$
echo "restored: $BASH_TRAPSIG"
unset BASH_TRAPSIG
( trap 'echo "EXIT: $BASH_TRAPSIG"' EXIT; true )

echo "=== read -E"
read -E line <<< "read with -E"
echo "[$line] $?"
read -rE -a parts <<< "a b c"
echo "${#parts[@]}"

echo "=== source -p"
mkdir -p $T/lib
echo 'echo "lib: $*"' > $T/lib/part.sh
echo 'echo "cwd file"' > $T/here.sh
cd $T
source -p lib part.sh one two
. -p "lib:." here.sh
. -p lib here.sh; echo "status $?"
source -p '' here.sh
source -p; echo "status $?"
source -x here.sh; echo "status $?"
PATH="$PWD/lib:$PATH" source part.sh from-path
shopt -u sourcepath
PATH="$PWD/lib:$PATH" source part.sh 2>&1 | sed 's/.*part.sh: //'
shopt -s sourcepath
cd ..

echo "=== printf %ls %lc %S %C"
export LC_ALL=C.UTF-8
printf '[%5ls][%-6ls][%.2ls][%5.1ls]\n' 'žluť' 'kůň' 'žluťoučký' 'ěšč'
printf '[%lc][%3lc][%-3lc][%S][%C]\n' 'éa' 'ž' 'ů' 'štěně' 'čap'
printf '[%5s][%.2s]\n' 'žluť' 'žluťoučký'
printf '[%*ls]\n' 6 'ďábel'
printf '[%lc]' '' | od -An -c | tr -s ' '
LC_ALL=C printf '[%5ls][%.2ls]\n' 'žluť' 'žluťoučký'
unset LC_ALL

echo "=== array_expand_once"
shopt -p array_expand_once assoc_expand_once
shopt -s assoc_expand_once
shopt -p array_expand_once assoc_expand_once
shopt -u array_expand_once
shopt -p assoc_expand_once

echo "=== patsub_replacement"
x=hello
echo "${x/l/[&]}" "${x//l/<\&>}" "${x/e/"&"}"
r='&&'
echo "${x/e/$r}" "${x/e/"$r"}"
shopt -u patsub_replacement
echo "${x/l/[&]}"
shopt -s patsub_replacement

rm -rf $T
