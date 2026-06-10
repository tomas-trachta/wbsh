#!/bin/bash
# Pins the control-flow signal semantics (break/continue/return/exit,
# set -e, set -u, ${x:?msg}, awk next/exit/loop flow) after the move
# from exception-based unwinding to value-based FlowSignal propagation.

echo "=== break N / continue N ==="
for i in 1 2 3; do
	for j in a b c; do
		[ "$j" = b ] && continue
		[ "$i" = 2 ] && [ "$j" = c ] && break 2
		echo "$i$j"
	done
done

echo "=== while break/continue ==="
n=0
while true; do
	n=$((n+1))
	[ $n -eq 2 ] && continue
	[ $n -ge 4 ] && break
	echo "n=$n"
done

echo "=== function return ==="
f() { return 42; echo not-reached; }
f; echo "rc=$?"
g() { for k in 1 2; do return 7; done; echo not-reached; }
g; echo "rc=$?"

echo "=== exit in subshell ==="
( exit 9 ); echo "subshell rc=$?"

echo "=== exit in cmd subst ==="
v=$(echo before; exit 3; echo after); echo "v=$v"

echo "=== set -e ==="
( set -e; false; echo not-reached ); echo "errexit rc=$?"
( set -e; if false; then :; fi; echo "cond ok" )
( set -e; false || echo "orside ok" )

echo "=== set -u ==="
( set -u; echo "$undefined_var_xyz" ); echo "after nounset"

echo "=== \${x:?msg} ==="
( echo "${undefined_var_xyz:?custom message}" ); echo "after qmark"

echo "=== awk flow ==="
printf 'a\nb\nc\n' | awk '{ if ($0 == "b") next; print "got " $0 }'
printf '1\n2\n3\n4\n' | awk '{ if ($0 == "3") exit 5; print $0 }'; echo "awk rc=$?"
printf '' | awk 'BEGIN { for (i = 1; i <= 5; i++) { if (i == 2) continue; if (i == 4) break; print i } }'
printf '' | awk 'BEGIN { n = 0; do { n++; if (n == 2) continue; print n } while (n < 4) }'

exit 0
