#!/bin/bash
# Line editor word keys, as readline binds them.
#
# Ctrl-W (unix-word-rubout) erases back to the previous whitespace, so it
# also takes the operators | > ; ( right before the cursor. Alt-Backspace
# (backward-kill-word), Alt-B, Alt-F and Alt-D use letters and digits as
# word characters. Output is [before cursor][after cursor].

echo "=== Ctrl-W right after an operator"
__lineedit 'ls >' '<C-w>'
__lineedit 'ls |' '<C-w>'
__lineedit 'a;' '<C-w>'
__lineedit 'echo (' '<C-w>'
__lineedit 'make &&' '<C-w>'

echo "=== Ctrl-W stops only at whitespace"
__lineedit 'echo a|b' '<C-w>'
__lineedit 'cat <in >out' '<C-w>' '<C-w>'
__lineedit 'cd /usr/local/bin' '<C-w>'
__lineedit 'echo hi;  ' '<C-w>'
__lineedit 'one two three' '<LEFT>' '<LEFT>' '<C-w>'
__lineedit '   ' '<C-w>'
__lineedit 'x' '<HOME>' '<C-w>'

echo "=== Alt-Backspace stops at punctuation"
__lineedit 'cd /usr/local/bin' '<M-BS>'
__lineedit 'cd /usr/local/bin/' '<M-BS>'
__lineedit 'foo_bar' '<M-BS>'
__lineedit 'ls >' '<M-BS>'
__lineedit 'git commit --amend' '<M-BS>' '<M-BS>'
__lineedit 'echo žluťoučký kůň' '<M-BS>'

echo "=== Alt-B and Alt-F move by the same words"
__lineedit 'git commit --amend' '<M-b>' 'X'
__lineedit 'git commit --amend' '<M-b>' '<M-b>' 'X'
__lineedit 'git commit --amend' '<HOME>' '<M-f>' 'X'
__lineedit 'git commit --amend' '<HOME>' '<M-f>' '<M-f>' 'X'
__lineedit 'a/b.c' '<M-b>' '<M-b>' '<M-f>' 'X'

echo "=== Alt-D erases the word after the cursor"
__lineedit 'git commit --amend' '<HOME>' '<M-d>'
__lineedit 'git commit --amend' '<HOME>' '<M-f>' '<M-d>'
