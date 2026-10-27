#!/bin/bash
# Probe: the EXIT trap of scripts, -c strings, subshells and $(...) (bash 5.2 semantics).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

echo "=== 1. -c string: trap fires when the string ends"
$W -r -c 'trap "echo bye" EXIT; echo hi'
echo "rc=$?"

echo "=== 2. exit N: \$? in the trap is N and the shell exits with N"
$W -r -c 'trap "echo \"bye \$?\"" EXIT; exit 3'
echo "rc=$?"

echo "=== 3. exit inside the trap overrides; bare exit keeps the status"
$W -r -c 'trap "echo in-trap; exit 7" EXIT; exit 3'
echo "rc=$?"
$W -r -c 'trap "false; exit" EXIT; exit 3'
echo "rc=$?"
$W -r -c 'trap "echo one; false" EXIT; exit 0'
echo "rc=$?"

echo "=== 4. set -e exit runs the trap"
$W -r -c 'set -e; trap "echo \"cleanup \$?\"" EXIT; echo a; false; echo not'
echo "rc=$?"

echo "=== 5. exit from a function"
$W -r -c 'f() { exit 4; }; trap "echo trap \$?" EXIT; f; echo not'
echo "rc=$?"

echo "=== 6. the latest trap wins; trap - and trap '' disable it"
$W -r -c 'trap "echo first" EXIT; trap "echo second" EXIT'
$W -r -c 'trap "echo x" EXIT; trap - EXIT; echo none'
$W -r -c 'trap "" EXIT; echo ignored'
$W -r -c 'trap "echo zero" 0; echo z'

echo "=== 7. the trap sees the final state and positionals"
$W -r -c 'trap "echo \$x \$1 \$0" EXIT; x=late' n1 a1
$W -r -c 'trap "trap -p" EXIT'

echo "=== 8. script file"
mkdir -p _run_tmp
cat > _run_tmp/exit_trap_child <<'EOF'
trap 'echo "child-bye $?"' EXIT
echo child
exit 6
EOF
$W -r _run_tmp/exit_trap_child
echo "rc=$?"

echo "=== 9. subshells keep their own EXIT trap"
trap 'echo parent-bye' EXIT
(echo sub)
(trap "echo sub-bye" EXIT; echo sub2)
(trap "exit 9" EXIT; true)
echo "sub rc=$?"

echo "=== 10. command substitution runs its own trap into the capture"
x=$(trap "echo cs-bye" EXIT; echo cs)
echo "x=[$x]"
y=$(echo plain)
echo "y=[$y]"

echo "=== 11. exec replaces the shell: no trap"
$W -r -c 'trap "echo bye" EXIT; exec echo replaced; echo not-reached'
echo "rc=$?"
$W -r -c 'trap "echo bye" EXIT; exec false; echo not-reached'
echo "rc=$?"

echo "=== 12. stdout redirected away, trap output on stderr"
$W -r -c 'trap "echo bye >&2" EXIT; echo out' > /dev/null

rm -f _run_tmp/exit_trap_child
echo "=== end (parent trap follows)"
