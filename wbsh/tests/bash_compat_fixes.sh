#!/bin/bash
# Divergences found by running GNU bash's own suites and real-world rc files.

WBSH=$(cd ../../x64/Release 2>/dev/null && pwd)/wbsh.exe
[ -x "$WBSH" ] || WBSH=wbsh
T=_compat_fixes_tmp
rm -rf $T
mkdir -p $T

echo "=== literal text is never split"
IFS=:; x=$(echo a:b); IFS=' '; echo "$x"
IFS=:; printf '[%s]' a:b $x; echo; IFS=' '

echo "=== unquoted \$* and \${a[*]} with an empty IFS"
"$WBSH" -c 'IFS=; A=(bob "tom dick" joe); set -- ${A[*]}; echo $#; set -- $*; echo $#'
"$WBSH" -c 'A=(bob "tom dick"); IFS=:; set -- a "" b; x=$*; y=${A[*]}; echo "$x/$y"'

echo "=== numeric operands"
for b in exit break continue shift; do
    BUILTIN=$b "$WBSH" -c 'set -- a b c; (exit 45); for f in _; do $BUILTIN abcde; done; echo after $BUILTIN: $?' bash
done
"$WBSH" -c 'f() { return abcde; echo in f; }; f; echo after return: $?' bash
"$WBSH" -c 'exit 1 2; echo after exit: $?' bash
"$WBSH" -c 'for i in 1; do break 0; done; echo after break 0: $?' bash
"$WBSH" -c 'set -- a; shift 2; echo "quiet: $?"; shopt -s shift_verbose; shift 2; echo "verbose: $?"; shift -1' bash

echo "=== set -k"
"$WBSH" -c 'set -k; f() { echo "a=$a b=$1"; }; f a=5 b; echo "a=${a-unset}"'

echo "=== aliases with shell syntax"
printf '%s\n' 'shopt -s expand_aliases' "alias L='m=(\"x\")'" 'L' 'declare -p m' \
    "alias P='echo a | tr a b'" 'P' "alias Q='q=1; echo \$q'" 'Q' > $T/alias.sh
"$WBSH" $T/alias.sh

echo "=== mapfile -C"
printf 'a\nb\nc\n' | "$WBSH" -c 'cb() { echo "cb $1 $2"; }; mapfile -t -C cb -c 2 arr; echo "${#arr[@]}"'

echo "=== declare -p of declared-only arrays"
"$WBSH" -c 'declare -A x; declare -p x; declare -a y; declare -p y; z=(); declare -p z; declare -A w=(); declare -p w; declare -a v; v[1]=1; declare -p v'

echo "=== unalias, hash, shopt -o"
"$WBSH" -c 'unalias -x foo; echo "status $?"'
"$WBSH" -c 'hash -p /bin/true mytrue; hash -t mytrue; hash -t nope; echo "status $?"; hash -d mytrue; hash'
"$WBSH" -c 'shopt -qo errexit; echo $?; set -u; shopt -qo nounset; echo $?; shopt -oq posix; echo $?; shopt -po nounset; shopt -o pipefail'
"$WBSH" -c 'shopt -p interactive_comments syslog_history'

echo "=== BASH_ARGV0"
"$WBSH" -c 'BASH_ARGV0=hello; echo "$0"'

echo "=== redirections"
(cd $T && "$WBSH" -c 'z="a b"; echo hi > $z; echo "status $?"; set -- "p q" r; cat <<< "$@"; x="s  t"; cat <<< $x')
(cd $T && "$WBSH" -c 'touch g1 g2; echo hi > g*; echo "status $?"')
echo piped | "$WBSH" -c 'cat /dev/stdin'

echo "=== set -r"
"$WBSH" -c 'set -r; echo "$-"; cd /; echo "cd $?"; echo x > /dev/null; echo "redirect $?"; ./x; echo "slash $?"; PATH=/x; echo not reached'

echo "=== \${a[@]+\"\${a[@]}\"} and upvars"
"$WBSH" -c 'arr=(a "b c"); printf "[%s]" ${arr[@]+"${arr[@]}"}; echo; set -- x "y z"; printf "[%s]" ${@:+"$@"}; echo'
"$WBSH" -c 'setter() { unset -v "$1"; eval "$1=\$2"; }; inner() { local v=i; local "$1" && setter "$1" x; }; outer() { local v=o; inner v; echo "$v"; }; outer; echo "global ${v-unset}"'
"$WBSH" -c 'f() { local v=1; unset v; echo "own local: [${v-unset}]"; }; v=g; f; echo "after: $v"'

echo "=== DIRSTACK"
"$WBSH" -c 'echo ${#DIRSTACK[@]}; [[ ${DIRSTACK[0]} == "$PWD" ]] && echo "top is PWD"'

echo "=== relative symlinks"
mkdir -p $T/d && echo linked > $T/t.txt
(cd $T && "$WBSH" -c 'ln -s ../t.txt d/l.txt 2>/dev/null && cat d/l.txt || echo "symlinks unavailable"')

rm -rf $T
