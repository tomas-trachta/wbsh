#!/bin/bash
# Probe: an assignment to a readonly variable drops the rest of the line, as in bash.

readonly R=1
echo "=== 1. a subshell ends at the failed assignment"
( R=2; echo "not reached" ) 2>&1; echo "rc=$?"
( R+=x; echo "not reached" ) 2>&1; echo "rc=$?"
declare -ra arr=(1 2)
( arr[0]=9; echo "not reached" ) 2>&1; echo "rc=$? ${arr[*]}"

exec 2>/dev/null
echo "=== 2. the rest of the line goes, the next line runs"
R=2; echo "not reached"
echo "rc=$?"
f() { R=3; echo "not reached in f"; }
f; echo "not reached after f"
echo "after f"
for i in 1 2; do
    echo "loop $i"
    R=4
    echo "not reached in loop"
done
echo "after loop"
x=$(R=5; echo "not reached"); echo "x=[$x] rc=$?"
if true; then R=6; echo "not reached in if"; fi; echo "not reached after if"
R=7 && echo "not reached in and" || echo "not reached in or"
echo "after and-or"

echo "=== 3. eval and source end, their caller goes on"
eval 'R=8; echo "not reached in eval"'; echo "after eval rc=$?"
echo 'R=9; echo "not reached in source"' > _run_tmp/readonly_discard.inc
. ./_run_tmp/readonly_discard.inc; echo "after source rc=$?"

echo "=== 4. other assignments only fail"
R=10 true; echo "prefix assignment rc=$?"
declare R=11; echo "declare rc=$?"
for R in a b; do echo "not reached in for"; done; echo "for rc=$?"
read -r R <<< x; echo "read rc=$?"
echo "R=$R"
