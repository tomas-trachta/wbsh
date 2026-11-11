#!/bin/bash
# `cmd --help` of a command without help of its own prints its synopsis
# from the manual table: utilities GNU style with status 0, shell builtins
# bash style with status 2. `yes --help` used to print "--help" for ever.
# echo, test and true take --help as an operand, as in bash.

show() {
    "$@" < /dev/null 2>&1 | head -n 4
    echo "status ${PIPESTATUS[0]}"
}

echo "=== utilities"
show yes --help
show cat --help
show watch --help

echo "=== shell builtins"
show cd --help
show read --help

echo "=== --help as an operand"
echo --help
test --help; echo "test $?"
true --help; echo "true $?"

echo "=== --help with other arguments is the command's own business"
printf 'x\n' | cat - --help 2>&1 | head -n 2
