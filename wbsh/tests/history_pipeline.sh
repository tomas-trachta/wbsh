#!/bin/bash
# A pipeline stage is a subshell of this shell, so it sees the shell's
# history and job table, as a forked bash subshell does.

history -c
history -s 'echo alpha'
history -s 'grep foo bar.txt'
history -s 'echo beta'

echo "=== history in a pipeline stage"
history | cat
history | grep foo
history | wc -l
echo "--- \$(history) agrees"
echo "$(history | grep beta)"

echo "=== a pipeline stage's history changes stay in the stage"
history -s 'only in the stage' | cat
history | grep -c 'only in the stage'

echo "=== functions and subshell groups in a stage see it too"
show_history() { history; }
show_history | tail -1
{ history; } | head -1
( history | grep alpha ) | cat

echo "=== an empty history stays empty"
history -c
history | wc -l

echo "=== jobs in a pipeline stage"
sleep 2 &
jobs | sed 's/   */ /g'
jobs | wc -l
wait
jobs | sed 's/   */ /g'
