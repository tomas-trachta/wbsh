#!/bin/bash
# Probe: "$@" / "${arr[@]}" field boundaries. Every element stays one
# field whatever IFS holds, and an empty list gives zero fields, not
# one empty field. Expected output matches bash 5.2.

show() {
    printf 'n=%d' $#
    for arg; do printf ' [%s]' "$arg"; done
    echo
}

none() {
    show "$@"
    show "${@}"
    show "$@"""
    show ''"$@"
    show x"$@"
    show "$@$@"
    show $@
    show "$*"
    local x=
    show "$x$@"
}

echo "=== 1. no positionals"
none

echo "=== 2. empty array"
e=()
show "${e[@]}"
show "${!e[@]}"
show "${e[*]}"
show ${e[@]}
show "${e[@]}${e[@]}"
for i in "${e[@]}"; do echo "  never [$i]"; done
echo "  loop done"

empties() {
    show "$@"
    show $@
    show x$@y
    show ''$@
    show "$*"
}

echo "=== 3. empty elements"
empties "" ""
empties ""

colon() {
    show "$@"
    show $@
    show "x$@y"
    show "$*"
    show $*
}

echo "=== 4. IFS=:"
IFS=:
colon a "b c" d
colon a "" "b:c"
a=(p "q r" s)
show "${a[@]}"
show ${a[@]}
show "${a[*]}"
show ${a[*]}
show "${!a[@]}"
for i in "${a[@]}"; do echo "  [$i]"; done

joined() {
    x="$@"
    echo "assign [$x]"
    x=$@
    echo "assign unquoted [$x]"
    x=$*
    echo "assign star [$x]"
    case $@ in
        "a b") echo "case space" ;;
        *) echo "case other" ;;
    esac
}

joined a b

echo "=== 5. IFS empty"
IFS=
quoted_empty_ifs() {
    show "$@"
    show $@
    show "$*"
}

quoted_empty_ifs a "b c" ""
show "${a[@]}"
show ${a[@]}
x="${a[@]}"
echo "assign [$x]"

echo "=== 6. default IFS"
IFS=$' \t\n'
defaults() {
    show "$@"
    show $@
    show x$@
}

defaults " a " "b"
forward() { show "$@"; }
forward "one two" "" three

echo "=== 7. unset IFS acts as default"
unset IFS
x="1 2	3"
show $x
unset_ifs() {
    show "$*"
    show $*
}
unset_ifs p "q r"
show "${a[*]}"
