#!/bin/bash
# Probe: when and how aliases expand, as bash does it in a script.

echo "=== 1. off in a script until expand_aliases"
alias greet='echo hello'
greet 2>/dev/null || echo "not expanded rc=$?"
shopt -q expand_aliases || echo "expand_aliases is off"
shopt -s expand_aliases
greet

echo "=== 2. only an unquoted name typed as such"
alias echo='echo [aliased]'
echo plain
\echo backslash
'echo' single
"echo" double
cmd=echo; $cmd expanded-name
unalias echo

echo "=== 3. a value ending in a blank makes the next word a candidate"
alias run='echo ' target=TARGET
run target target
run run target
alias inner='echo ' outer='inner'
outer target
alias empty=''
empty run target

echo "=== 4. an alias does not expand itself again"
alias ls='ls-alias-not-run'
alias self='self-again'
self 2>/dev/null || echo "self rc=$?"
unalias ls self

echo "=== 5. listing"
alias quoted="say 'it'"
alias -p | while IFS= read -r line; do case $line in *quoted*|*target*) echo "$line" ;; esac; done
alias quoted
alias -- -dash='echo dash'
alias -- -dash
alias -z 2>&1; echo "invalid option rc=$?"
unalias quoted
alias quoted 2>&1; echo "unaliased rc=$?"

echo "=== 6. a subshell and a pipeline stage keep the setting"
( greet )
greet | cat
x=$(greet); echo "x=$x"
