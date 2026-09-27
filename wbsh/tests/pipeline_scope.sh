#!/bin/bash
# Probe: pipeline subshell semantics. The classic foot-gun is that a
# `while read | ...` loop runs in a subshell and parent vars are NOT
# updated. This must match bash exactly or scripts that rely on the
# subshell isolation will silently corrupt state.

echo "=== 1. while-read pipeline (bash: counter stays 0)"
counter=0
seq 1 5 | while read n; do
    counter=$((counter + 1))
done
echo "after pipe: counter=$counter (bash expects 0)"

echo "=== 2. command substitution captures the count fine"
counter=$(seq 1 5 | wc -l | tr -d ' ')
echo "via cmdsubst: counter=$counter (bash expects 5)"

echo "=== 3. explicit subshell isolates"
x=outer
( x=inner; echo "  inside: $x" )
echo "after subshell: x=$x (bash expects outer)"

echo "=== 4. brace group does NOT isolate"
y=outer
{ y=inner; }
echo "after brace: y=$y (bash expects inner)"

echo "=== 5. exit code propagation through pipeline"
false | true
echo "false|true rc=$?  (bash expects 0)"

echo "=== 6. set -o pipefail surfaces left-side failure"
set -o pipefail
false | true
echo "false|true rc=$?  (with pipefail, expects 1)"
set +o pipefail

echo "=== 7. nested pipeline + cmd subst"
result=$(echo "a b c d" | tr ' ' '\n' | wc -l | tr -d ' ')
echo "nested cmdsubst: $result (bash expects 4)"

echo "=== done"
