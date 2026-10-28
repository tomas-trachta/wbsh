#!/bin/bash
# timeout starts the command inside its kill-on-close job before the command
# runs, so nothing it spawns escapes; -k / --kill-after first sends a
# Ctrl+Break the command may survive (ping does) and KILL after the grace
# period; option errors exit 125 as in GNU coreutils.

echo "=== plain expiry"
timeout 1 sleep 5
echo "expired: $?"
timeout -v 1 sleep 5
echo "verbose: $?"
timeout -s KILL 1 sleep 5
echo "KILL: $?"
timeout 5 true
echo "fast command: $?"
timeout 5 sh -c 'exit 3'
echo "own status: $?"

echo "=== -k / --kill-after"
timeout -k 5 1 sleep 5
echo "obeys Ctrl+Break: $?"
timeout -v -k 1 1 ping -t 127.0.0.1 > /dev/null
echo "survives Ctrl+Break: $?"
timeout --kill-after=1 -s INT 1 ping -t 127.0.0.1 > /dev/null
echo "long option: $?"
timeout -k 1 -s KILL 1 sleep 5
echo "KILL needs no grace: $?"

echo "=== nothing escapes the job"
spawner() {
    sleep 43.75 &
    sleep 43.75
}
timeout 1 spawner
echo "spawner: $?"
sleep 0.5
pgrep -f 'sleep 43[.]75' > /dev/null || echo "no grandchild survived"

echo "=== errors"
timeout -k x 1 sleep 1
echo "bad -k: $?"
timeout -s FOO 1 sleep 1
echo "bad signal: $?"
timeout 1
echo "missing command: $?"
timeout -1 sleep 1
echo "negative: $?"
timeout 2 nosuchcommand
echo "not found: $?"
