#!/bin/bash
# Job specs (%N, %%, %-, %name, %?text) for kill / wait / fg / disown,
# bash's `jobs` layout, `wait -n`, `bg` without job control, and Ctrl+C
# stopping loops and `sleep` (pressed through the `__ctrlc` test hook).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

echo "=== kill and wait with job specs"
sleep 0 & wait
sleep 5 & kill %1; wait %1; echo "status $?"
sleep 5 & sleep 5 & kill %% ; kill %-; wait; echo "both gone"
sleep 5 & kill %sle; wait $!; echo "by name $?"
sleep 5 & kill %?eep; wait $!; echo "by substring $?"
kill %9; echo "no job $?"

echo "=== jobs"
sleep 1 & sleep 2 &
jobs
jobs -p | wc -l
jobs -l | sed 's/^\(\[[0-9]*\][-+ ]\) *[0-9][0-9]* /\1 PID /'
wait
jobs | wc -l

echo "=== wait -n"
sleep 3 & sleep 2 & (exit 3) &
wait -n; echo "first done $?"
wait
wait -n; echo "nothing left $?"

echo "=== a finished job is reported once"
(exit 5) &
pid=$!
sleep 1
jobs
jobs | wc -l
wait $pid; echo "late wait $?"

echo "=== bg without job control"
bg; echo "bg rc $?"

echo "=== Ctrl+C stops sleep and loops"
timeout 20 $W -r -c '__ctrlc 300; while true; do sleep 1; done; echo not reached'
echo "endless sleep loop rc=$?"
timeout 20 $W -r -c '__ctrlc 300; sleep 10'
echo "sleep rc=$?"
timeout 20 $W -r -c '__ctrlc 500; for i in 1 2 3 4 5 6 7 8 9; do sleep 0.4; echo $i; done'
echo "for loop rc=$?"
timeout 20 $W -r -c '__ctrlc 200; while :; do :; done'
echo "busy loop rc=$?"
