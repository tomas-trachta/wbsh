#!/bin/bash
# Signals between wbsh processes: kill, pkill and timeout post them into
# the target shell's mailbox, and it runs its trap, ignores them, or ends
# as the default action says. Other processes are terminated as before.
# Also kill's signal names (-NAME, -SIGNAME, lowercase, -s, -n, -l).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

W=$PWD/$W
T=_signals_tmp
rm -rf $T
mkdir -p $T
cd $T

# Starts a wbsh running $1 in the background; it records its PID first.
started() {
	rm -f "$1.pid"
	$W -r -c "echo \$\$ > $1.pid; $2" &
	while [ ! -s "$1.pid" ]; do sleep 0.05; done
	sleep 0.2
}

echo "=== to itself"
$W -r -c 'trap "echo trapped USR1" USR1; kill -USR1 $$; echo after USR1'
$W -r -c 'trap "" HUP; kill -HUP $$; echo HUP ignored'
$W -r -c 'kill -s usr2 $$; echo not reached'; echo "untrapped USR2 status $?"
$W -r -c 'trap "echo EXIT trap" EXIT; kill $$; echo not reached'; echo "TERM status $?"
$W -r -c 'kill -CHLD $$; kill -WINCH $$; echo CHLD and WINCH ignored by default'

echo "=== from another shell"
started a 'trap "echo A: reloading" HUP; trap "echo A: cleanup; exit 7" TERM; while :; do sleep 0.1; done'
pid=$!
kill -HUP "$(cat a.pid)"; sleep 0.4
kill -SIGHUP "$(cat a.pid)"; sleep 0.4
kill -term "$(cat a.pid)"
wait $pid; echo "A status $?"

started b 'trap "echo B: EXIT trap" EXIT; sleep 30'
pid=$!
kill "$(cat b.pid)"
wait $pid; echo "B status $?"

echo "=== wait is cut short, sleep is not"
started c 'trap "echo C: USR1" USR1; sleep 5 & wait $!; echo "C: wait returned $?"; kill %1'
pid=$!
kill -USR1 "$(cat c.pid)"
wait $pid; echo "C status $?"
started d 'trap "echo D: USR1" USR1; sleep 0.8; echo "D: sleep returned $?"'
pid=$!
kill -USR1 "$(cat d.pid)"
wait $pid; echo "D status $?"

echo "=== the command a shell waits for"
started e 'trap "echo E: TERM after ping" TERM; ping -n 2 127.0.0.1 > /dev/null; echo E: done'
pid=$!
kill "$(cat e.pid)"
wait $pid; echo "E status $?"
started f 'ping -n 30 127.0.0.1 > /dev/null; echo F: not reached'
pid=$!
kill "$(cat f.pid)"
wait $pid; echo "F status $?"
ping -n 30 127.0.0.1 > /dev/null &
pid=$!
sleep 0.3
kill $pid
wait $pid; echo "background command status $?"

started h 'tail -f h.pid > /dev/null'
pid=$!
kill "$(cat h.pid)"
wait $pid; echo "shell busy in a builtin status $?"

echo "=== pkill and timeout"
started g 'trap "echo G: USR2" USR2; sleep 1'
pid=$!
pkill -USR2 -f 'G: USR2'
wait $pid; echo "G status $?"
timeout 1 $W -r -c 'trap "echo child: TERM; exit 3" TERM; while :; do sleep 0.1; done'
echo "timeout status $?"
timeout --preserve-status 1 $W -r -c 'trap "exit 3" TERM; while :; do sleep 0.1; done'
echo "preserved status $?"
timeout -k 1 1 $W -r -c 'trap "" TERM; while :; do sleep 0.1; done'
echo "ignored TERM, then KILL: status $?"

echo "=== other processes are terminated"
ping -n 30 127.0.0.1 > /dev/null &
pid=$!
sleep 0.3
kill -USR1 $pid
wait $pid; echo "ping status $?"

echo "=== kill arguments"
kill -l | head -2
kill -l | tail -1
kill -l 15 143 TERM sigusr1 SIGKILL
kill -USR9 $$; echo "status $?"
kill -s NOPE $$; echo "status $?"
kill -s; echo "status $?"
kill -n 0 $$ && echo "-n 0 probes"
kill -0 -- $$ && echo "-- ends the options"
kill; echo "status $?"
trap -l | tail -1

cd ..
rm -rf $T
