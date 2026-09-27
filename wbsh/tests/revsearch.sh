#!/bin/bash
# Reverse-incremental-search matcher tests.
#
# Drives the pure `findReverseSearchMatch` helper that powers Ctrl-R in
# the interactive line editor via the `__revsearch` test hook. The
# interactive modal itself needs a real TTY and isn't exercised here.

history -c
history -s "echo one"
history -s "ls -la"
history -s "echo two"
history -s "echo three"
history -s "grep foo"

echo "=== default (latest) match"
__revsearch echo
__revsearch ls
__revsearch grep

echo "=== continue backward via -c"
__revsearch -c 3 echo
__revsearch -c 2 echo
__revsearch -c 1 echo

echo "=== forward (-f) selects the earliest match at or after -c"
__revsearch -f -c 1 echo
__revsearch -f -c 2 echo
__revsearch -f -c 3 echo
__revsearch -f -c 4 echo

echo "=== misses"
__revsearch nothing
__revsearch ""
__revsearch -f -c 6 echo

echo "=== substring (not prefix) and entry boundaries"
history -c
history -s "foo bar"
history -s "foobar"
history -s "bar baz"
__revsearch bar
__revsearch foo
__revsearch -c 1 bar
__revsearch -f -c 1 baz

echo "=== empty history"
history -c
__revsearch anything

echo "=== exit status"
history -s "alpha"
__revsearch alpha && echo "hit rc=$?"
__revsearch missing; echo "miss rc=$?"

echo "=== history -s joins extra args with a space"
history -c
history -s echo combined args
__revsearch combined

echo "=== done"
