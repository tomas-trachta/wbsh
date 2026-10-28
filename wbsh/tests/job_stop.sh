#!/bin/bash
# Stopped jobs: `kill -STOP` / `-TSTP` suspend a job (every process of
# it), `jobs` shows it Stopped, `kill -CONT` resumes it, and a shell that
# exits ends the jobs it leaves stopped. Ctrl+Z at an interactive prompt
# stops the foreground job the same way (checked in a pseudoconsole).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

echo "=== kill -STOP and -CONT on a job"
sleep 3 &
kill -STOP %1; echo "stop rc=$?"
jobs
kill -CONT %1; echo "cont rc=$?"
jobs
kill %1; wait %1; echo "killed $?"

echo "=== by PID, -TSTP and a pipeline"
sleep 3 | sleep 3 &
kill -TSTP $!
jobs
kill -18 $!
jobs
kill -9 %1; wait; echo "pipeline gone"

echo "=== a stopped job does not outlive its shell"
pid=$(timeout 20 $W -r -c 'sleep 30 & kill -STOP %1; echo $!')
sleep 0.5
kill -0 $pid 2>/dev/null && echo "still there" || echo "ended with the shell"

echo "=== kill -l lists the job signals"
kill -l | grep -c STOP
