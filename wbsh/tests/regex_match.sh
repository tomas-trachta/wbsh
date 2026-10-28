#!/bin/bash
# Probe: [[ string =~ regex ]] parsing, quoting and BASH_REMATCH (bash 5.2 semantics).

echo "=== 1. unquoted ( ) | belong to the regex word"
[[ abc =~ ^a(b)c$ ]] && echo "group: ${BASH_REMATCH[*]} (${#BASH_REMATCH[@]})"
[[ foo =~ ^(foo|bar)$ ]] && echo "alternation: ${BASH_REMATCH[1]}"
[[ ab =~ a|z ]] && echo "bare bar"
[[ "a b" =~ (a b) ]] && echo "blank inside parens: [${BASH_REMATCH[1]}]"
[[ 2024-01-05 =~ ^([0-9]{4})-([0-9]{2})-([0-9]{2})$ ]] && echo "date: ${BASH_REMATCH[1]} ${BASH_REMATCH[3]}"
[[ a =~ ^[[:alpha:]]$ ]] && echo "bracket class"
[[ abc =~ b && 1 -eq 1 ]] && echo "&& after the regex"
[[ abc =~ z || 1 -eq 1 ]] && echo "|| after the regex"
f() { [[ $1 =~ ^-(v+)$ ]] && echo "verbosity ${#BASH_REMATCH[1]}"; }
f -vvv
echo [[ =~ "(x)"

echo "=== 2. quoted parts match literally"
re='a.c'
[[ abc =~ "$re" ]] || echo "quoted variable is literal"
[[ a.c =~ "$re" ]] && echo "quoted variable matches itself"
[[ abc =~ $re ]] && echo "unquoted variable is a regex"
[[ abc =~ ^a"."c$ ]] || echo "quoted dot is literal"
[[ a.b =~ a\.b ]] && echo "escaped dot matches a dot"
[[ axb =~ a\.b ]] || echo "escaped dot is not a wildcard"
[[ "a|b" =~ a\|b ]] && echo "escaped bar is literal"
[[ 'x+y' =~ 'x+y' ]] && echo "single-quoted plus is literal"

echo "=== 3. BASH_REMATCH"
[[ abc =~ a(x)?bc ]] && echo "unmatched group: [${BASH_REMATCH[1]}] (${#BASH_REMATCH[@]})"
[[ xyz =~ ^(foo|bar)$ ]] || echo "no match leaves ${#BASH_REMATCH[@]} elements"
[[ ab =~ (a)(b) ]]
declare -p BASH_REMATCH
shopt -s nocasematch
[[ ABC =~ ^a(b)c$ ]] && echo "nocasematch: ${BASH_REMATCH[1]}"
shopt -u nocasematch

echo "=== 4. invalid regex returns 2"
bad='a('
[[ abc =~ $bad ]]
echo "status $?"
[[ abc =~ $bad ]] || echo "and is false"
