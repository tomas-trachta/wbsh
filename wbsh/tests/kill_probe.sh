#!/bin/bash
# kill -0 / pkill -0 / killall -0 only probe; they must leave the target running.

ping -n 4 127.0.0.1 > /dev/null &
pid=$!
sleep 0.5

kill -0 "$pid"
echo "kill -0: $?"
kill -s 0 "$pid"
echo "kill -s 0: $?"
pkill -0 -i -x ping
echo "pkill -0: $?"
killall -0 PING.EXE
echo "killall -0: $?"

sleep 0.2
kill -0 "$pid"
echo "still running: $?"
pgrep -i -x ping > /dev/null
echo "ping still running: $?"

kill "$pid"
echo "kill: $?"
wait "$pid" 2> /dev/null

kill -0 "$pid" 2>&1 | sed "s/$pid/PID/"
kill -0 "$pid" 2> /dev/null
echo "after kill: $?"
