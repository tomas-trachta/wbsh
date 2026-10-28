#!/bin/bash
# Line editor: Ctrl-C abandons the line instead of editing on.
#
# `__lineedit` reports "interrupt" when the keys end the line with Ctrl-C;
# the REPL then drops any pending continuation (PS2) input, runs the INT
# trap and sets $? to 130. Keys after the Ctrl-C are never read.

echo "=== Ctrl-C ends the line"
__lineedit '<C-c>'
__lineedit 'echo "abc' '<C-c>'
__lineedit 'ab' '<LEFT>' '<C-c>' 'ls' '<ENTER>'

echo "=== Ctrl-C in reverse search ends the line too; Ctrl-G only leaves the search"
history -c
history -s 'echo hello'
history -s 'ls -la'
__lineedit 'x' '<C-r>' 'hel' '<C-c>'
__lineedit 'x' '<C-r>' 'hel' '<C-g>'
__lineedit 'x' '<C-r>' 'hel' '<ESC>'
__lineedit 'x' '<C-r>' 'hel' '<ENTER>'
__lineedit '<C-r>' 'l' '<C-r>' '<RIGHT>'

echo "=== the other line endings are unchanged"
__lineedit 'echo hi' '<ENTER>'
__lineedit '<ENTER>'
__lineedit '<C-d>'
__lineedit 'ab' '<C-d>'
__lineedit '<UP>' '<UP>' '<DOWN>'
