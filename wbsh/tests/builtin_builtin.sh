#!/bin/bash
# The `builtin` builtin: run a shell builtin even when a function or alias shadows it.

echo "=== wrapping cd in a same-named function"
start=$PWD
cd() { builtin cd "$@" && echo "wrapped cd ok"; }
cd ..
[ "$PWD" != "$start" ] && echo "directory changed"
unset -f cd
builtin cd "$start"
[ "$PWD" = "$start" ] && echo "back at start"

echo "=== plain use"
builtin echo hi
builtin printf '%s\n' a b
x=$(builtin pwd)
[ "$x" = "$PWD" ] && echo "pwd in substitution ok"
builtin echo piped | builtin read -r line; echo "read status $?"

echo "=== skips functions and aliases"
echo() { builtin echo "wrapped: $*"; }
echo through the function
builtin echo past the function
unset -f echo
alias say='echo ALIAS'
builtin echo past the alias
unalias say

echo "=== errors"
builtin nosuch; echo "status $?"
f() { :; }
builtin f; echo "status $?"
builtin -x echo; echo "status $?"
builtin; echo "status $?"
builtin --; echo "status $?"
builtin -- echo after double dash

echo "=== type and control flow"
builtin type -t builtin
g() { builtin return 4; echo unreachable; }
g; echo "return status $?"
(builtin exit 5); echo "exit status $?"
for i in 1 2 3; do builtin break; done; echo "break left i=$i"
