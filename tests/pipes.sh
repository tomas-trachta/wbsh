#!/bin/bash
# Concurrent-pipeline smoke tests.

echo "=== 1. Basic external chain"
echo -e "alpha\nbeta\ngamma\ndelta" | grep -v beta | sort -r

echo "=== 2. Builtin head + external tail"
echo "from-builtin" | cat | tr a-z A-Z

echo "=== 3. Producer fast, consumer slow (streaming)"
# seq generates 100 lines; head -3 stops early. The producer should NOT have
# to fill its entire output buffer before head exits — that'd indicate
# concurrent execution.
seq 1 100 | head -3

echo "=== 4. Head-of-pipe is a builtin"
{ echo a; echo b; echo c; echo d; echo e; } | tail -2

echo "=== 5. Compound (for) at head of pipeline"
for i in 1 2 3 4 5; do
  echo "row $i $((i*i))"
done | grep '4'

echo "=== 6. Triple pipe with mixed builtin / external"
echo "shared-input" | cat | wc -c

echo "=== 7. |& (stderr-to-stdout)"
{ echo to-out; echo to-err 1>&2; } |& sort

echo "=== 8. Pipeline exit status"
true | true | false; echo "after false: \$?=$?"
false | true | true; echo "after true:  \$?=$?"

echo "=== 9. Pipefail-ish with !"
! false; echo "! false -> \$?=$?"
! true;  echo "! true  -> \$?=$?"

echo "=== 10. Bigger throughput"
seq 1 1000 | wc -l

echo "=== done"
