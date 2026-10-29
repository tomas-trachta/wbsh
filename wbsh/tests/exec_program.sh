#!/bin/bash
# exec of an external program: Windows has no execve, so the program runs
# as a child the shell ends with. It does end with the program's own exit
# code, and it runs in a job that dies with the shell.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
	WIN='..\..\x64\Release\wbsh.exe'
else
	W=../../x64/Debug/wbsh.exe
	WIN='..\..\x64\Debug\wbsh.exe'
fi

T=_exec_program_tmp
rm -rf $T
mkdir -p $T

echo "=== statuses as the shell sees them"
$W -r -c 'exec cmd /c exit 7; echo not reached'; echo "status $?"
$W -r -c 'exec cmd /c exit 1000'; echo "status $?"
$W -r -c '( exec cmd /c exit 5 ); echo "subshell status $?"; exit 3'; echo "status $?"
$W -r -c 'trap "echo EXIT trap ran" EXIT; exec cmd /c echo from-program'; echo "status $?"
$W -r -c 'exec no-such-program-wbsh; echo not reached'; echo "status $?"
$W -r -c 'f() { echo in-function; return 4; }; exec f; echo not reached'; echo "status $?"
$W -r -c 'exec 3> /dev/null; echo redirection-only exec carries on'; echo "status $?"
$W -r -c 'exec cmd /c echo to-file > '$T'/out.txt'; cat $T/out.txt

echo "=== the exit code the parent sees"
powershell.exe -NoProfile -Command "
	& '$WIN' -r -c 'exec cmd /c exit 1000'; 'exit 1000 -> ' + \$LASTEXITCODE
	& '$WIN' -r -c 'exec cmd /c exit -1073741819'; 'access violation -> 0x{0:X8}' -f \$LASTEXITCODE
	& '$WIN' -r -c '( exec cmd /c exit 1000 ); exit 3'; 'subshell then exit 3 -> ' + \$LASTEXITCODE
" | tr -d '\r'

echo "=== the program dies with its shell"
$W -r -c "echo \$\$ > $T/wrapper.pid; exec ping -n 30 127.0.0.1" > /dev/null &
for i in 1 2 3 4 5 6 7 8 9 10; do
	[ -s $T/wrapper.pid ] && break
	sleep 0.2
done
sleep 0.5
wrapper=$(cat $T/wrapper.pid)
program=$(ps -e -o pid,ppid,comm | awk -v w="$wrapper" '$2 == w && tolower($3) ~ /ping/ { print $1 }')
[ -n "$program" ] && echo "ping runs under the wrapper"
kill -9 "$wrapper"
wait
sleep 0.5
ps -p "$program" > /dev/null 2>&1 && echo "ping outlived its wrapper" || echo "ping ended with its wrapper"

echo "=== what the program leaves behind outlives it"
$W -r -c "exec cmd /c \"start /b cmd /c ping -n 2 127.0.0.1 ^>nul ^& echo survived ^> $T\\survivor.txt\""
for i in $(seq 1 30); do
	[ -s $T/survivor.txt ] && break
	sleep 0.2
done
tr -d ' \r' < $T/survivor.txt

rm -rf $T
