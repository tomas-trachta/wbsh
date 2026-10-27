#!/bin/bash
# Probe: exit statuses are folded into 0..255 like bash does on POSIX.

echo "=== 1. process exit codes keep their low byte"
for code in 0 1 255 256 300 511 -1 -2 -256; do
	cmd /c "exit $code"
	echo "$code -> $?"
done

echo "=== 2. crash and Ctrl+C codes become 128 + signal"
for code in -1073741510 -1073741819 -1073741571 -1073741795 -1073741676 -1073740791; do
	cmd /c "exit $code"
	echo "$code -> $?"
done

echo "=== 3. pipelines"
cmd /c "exit 300" | cat
echo "last stage -> $?"
cat /dev/null | cmd /c "exit 300"
echo "last stage native -> $?"
set -o pipefail
cmd /c "exit 300" | cat
echo "pipefail -> $?"
set +o pipefail

echo "=== 4. wait"
{ cmd /c "exit 300" & } 2>/dev/null
wait $!
echo "wait pid -> $?"
{ cmd /c "exit -1073741510" & } 2>/dev/null
wait $!
echo "wait ctrl-c -> $?"

echo "=== 5. exit and return operands"
(exit 300); echo "exit 300 -> $?"
(exit -1); echo "exit -1 -> $?"
(exit 256); echo "exit 256 -> $?"
f() { return 257; }
f; echo "return 257 -> $?"
g() { return -2; }
g; echo "return -2 -> $?"
