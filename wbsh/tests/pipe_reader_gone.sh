#!/bin/bash
# Once the reader of a pipe exits, writers stop as SIGPIPE stops them on
# POSIX: filters and producers end instead of running forever. Each case
# runs under timeout, so a regression shows as status 124 rather than a hang.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

run() {
    timeout 20 "$WBSH" -r -c "$1"
    echo "status $?"
}

echo "=== filters between an endless producer and head"
run 'yes abc | sed s/a/b/ | head -2'
run 'yes | grep y | head -1'
run "yes abc | awk '{ print }' | head -1"
run 'yes abc | cat | head -1'
run 'yes abc | cat -n | head -1'
run 'yes abc | fold -w2 | head -1'
run 'yes abc | expand | head -1'
run 'yes abc | unexpand | head -1'
run 'yes abc | tr a b | head -1'
run 'yes abc | cut -c1 | head -1'
run 'yes abc | rev | head -1'
run 'yes abc | paste - - | head -1'
run 'yes abc | nl | head -1'
run 'yes | tee | head -1'
run 'yes | head -n 100000000 | head -1'
run 'yes | tail -n +1 | head -1'
run 'yes abcd | strings | head -1'
run 'seq 1 1000000000 | od -c | head -1'
run 'seq 1 1000000000 | uniq | head -1'

echo "=== producers"
run 'seq 1 1000000000 | head -2'
run "awk 'BEGIN { while (1) print \"y\" }' | head -1"
run 'yes | while read -r l; do echo "$l"; done | head -1'

echo "=== the writer reports SIGPIPE's status"
run 'set -o pipefail; seq 1 1000000000 | head -1'
run 'set -o pipefail; yes | sed p | head -1'

echo "=== a failed write elsewhere does not end the shell"
echo lost > /dev/full
echo "after /dev/full: $?"
{ echo lost; } > /dev/full
echo "still running"
