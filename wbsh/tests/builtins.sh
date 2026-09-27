#!/bin/bash
# Smoke test for the shell builtins surface.

echo "=== printf"
printf 'plain\n'
printf '%s %s\n' hello world
printf '%d * %d = %d\n' 6 7 42
printf '%-5s|%5s\n' a b
printf '%05d\n' 42
printf '%x %X %o\n' 255 255 8
printf 'esc: \t\\n\n'
printf '%s\n' a b c   # format reused over multiple args

echo "=== echo flags"
echo -n no-newline
echo
echo -e 'tab\there\nnewline'
echo -E 'literal\there'

echo "=== test / [ ]"
[ 1 -lt 2 ] && echo "1<2"
[ 2 -le 2 ] && echo "2<=2"
[ 3 -ne 2 ] && echo "3!=2"
[ "abc" = "abc" ] && echo "str eq"
[ "abc" != "xyz" ] && echo "str ne"
[ -z "" ] && echo "empty"
[ -n "x" ] && echo "nonempty"
[ "abc" \< "abd" ] && echo "lex"
test 1 -eq 1 && echo "test eq"

echo "=== type / command -v"
type echo
type cd
type pwd
command -v ls

echo "=== eval"
eval 'echo "evaluated:"' 'echo from eval'
x=hello
eval "echo \$x"

echo "=== shift / set --"
set -- a b c d
echo "args: $#"
echo "1=$1 2=$2 3=$3 4=$4"
shift
echo "after shift: 1=$1 2=$2 3=$3"
shift 2
echo "after shift 2: 1=$1"

echo "=== getopts"
parse_opts() {
    local OPTIND=1
    local opt
    local v=0
    local f=
    while getopts "vf:" opt; do
        case $opt in
            v) v=1 ;;
            f) f=$OPTARG ;;
        esac
    done
    echo "v=$v f=$f"
}
parse_opts -v -f hello
parse_opts -f only
parse_opts -v

echo "=== local / declare"
counter() {
    local n=0
    n=$((n + 1))
    echo "in func: n=$n"
}
n=outer
counter
echo "outer: n=$n"

echo "=== alias"
alias greet='echo hi'
greet there
unalias greet

echo "=== source"
cat > _source_t.sh <<'EOF'
SOURCED_VAR=from-source
sourced_fn() { echo "sourced fn: $1"; }
EOF
. ./_source_t.sh
echo "$SOURCED_VAR"
sourced_fn ok
rm -f _source_t.sh

echo "=== let"
let "a = 2 + 3"
echo "a=$a"
let "b = a * 2"
echo "b=$b"

echo "=== done"
