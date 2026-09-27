#!/bin/bash
# Inline-prediction matcher tests.
#
# Drives the pure `findInlinePrediction` helper that powers PowerShell-
# style ghost-text suggestions in the interactive line editor via the
# `__predict` test hook. The visible rendering and Right-Arrow accept
# need a real TTY and aren't exercised here.

history -c
history -s "ls"
history -s "ls -la"
history -s "echo one"
history -s "claude"
history -s "claude --dangerously-skip-permissions"

echo "=== suggests latest matching entry's suffix"
__predict claude
__predict ls
__predict echo

echo "=== empty prefix yields no prediction"
__predict ""

echo "=== exact match yields no prediction (no strict extension)"
__predict "claude --dangerously-skip-permissions"

echo "=== prefix nobody starts with"
__predict nothing

echo "=== ties resolve to the newest entry"
history -c
history -s "ls"
history -s "ls -la"
history -s "ls /tmp"
history -s "ls /var"
__predict ls

echo "=== empty history"
history -c
__predict anything

echo "=== exit status"
history -s "alpha beta"
__predict alpha && echo "hit rc=$?"
__predict missing; echo "miss rc=$?"

echo "=== failed entries are filtered out"
history -c
history -s "make build"
history -s "make typo"
history -s "make test"
__histstat 2 1
__predict make
# Marking it back to success makes it eligible again. Newest wins, so
# the original 'make test' is still preferred over 'make typo'.
__histstat 2 0
__predict make
# Mark every match as failed -> no prediction left.
__histstat 1 1
__histstat 2 1
__histstat 3 1
__predict make

echo "=== filter only hides matches that strictly fail"
history -c
history -s "claude --foo"
history -s "claude --bar"
__histstat 2 127
__predict claude

echo "=== done"
