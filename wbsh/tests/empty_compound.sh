#!/bin/bash
# Compound commands need a command in each list, as in bash: an empty
# `then`, `do` or brace body is a syntax error (status 2), not an empty
# command -- `while do done` used to loop for ever.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

try() {
    timeout 10 "$WBSH" -r -c "$1" > /dev/null 2>&1 < /dev/null
    echo "[$1] status $?"
}

echo "=== empty lists are syntax errors"
try 'if then fi'
try 'if true; then fi'
try 'if true; then :; elif then :; fi'
try 'if true; then :; else fi'
try 'while do done'
try 'until false; do done'
try 'for x in a; do done'
try '{ }'
try '( )'
try 'f() { }'

echo "=== non-empty lists and empty case items still work"
try 'if true; then :; fi'
try 'while false; do :; done'
try 'case x in x) ;; esac'
try '{ :; }'
