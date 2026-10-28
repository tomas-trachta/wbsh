#!/bin/bash
# Ctrl-R reverse-i-search inside the line editor, driven by `__lineedit`:
# the query takes any character, the key that ends the search does its
# own job on the match, and a query that matches nothing keeps the last
# match and says so.

history -c
history -s 'echo žluťoučký kůň'
history -s 'ls -la /tmp'
history -s 'echo hello world'
history -s 'grep foo bar.txt'

echo "=== the cursor sits where the query starts in the match"
__lineedit '<C-r>' 'ls' '<ESC>'
__lineedit '<C-r>' 'world' '<ESC>'

echo "=== non-ASCII characters extend the query"
__lineedit '<C-r>' 'kůň' '<ESC>'
__lineedit '<C-r>' 'ž' '<ESC>'

echo "=== the key that ends the search runs on the match"
__lineedit '<C-r>' 'hello' '<LEFT>' 'X'
__lineedit '<C-r>' 'hello' '<C-a>' '#'
__lineedit '<C-r>' 'hello' '<END>' '!'
__lineedit '<C-r>' 'hello' '<C-e>' '<BS>'

echo "=== Enter runs the match; Ctrl-G goes back to the line typed before"
__lineedit '<C-r>' 'grep' '<ENTER>'
__lineedit 'typed' '<C-r>' 'grep' '<C-g>'

echo "=== the search starts on the line typed so far"
__lineedit -o -p '$ ' 'partial' '<C-r>'

echo "=== a failing query keeps the last match"
__lineedit '<C-r>' 'hellx' '<ESC>'
__lineedit -o -p '$ ' '<C-r>' 'hex'
__lineedit '<C-r>' 'hellx' '<BS>' 'o w' '<ESC>'
__lineedit 'typed' '<C-r>' 'zzz' '<ESC>'

echo "=== Ctrl-R steps to older matches, Ctrl-S back to newer ones"
__lineedit '<C-r>' 'echo' '<C-r>' '<ESC>'
__lineedit '<C-r>' 'echo' '<C-r>' '<C-r>' '<ESC>'
__lineedit '<C-r>' 'echo' '<C-r>' '<C-s>' '<ESC>'

echo "=== Ctrl-R on an empty query searches for the last query again"
__lineedit '<C-r>' 'echo' '<C-r>' '<ESC>' '<C-u>' '<C-r>' '<C-r>' '<ESC>'

echo "=== a match that wraps is erased whole when the search moves on"
history -s 'echo this is a rather long command line that wraps on a narrow screen'
__lineedit -o -c 20 -p '$ ' '<C-r>' 'narrow' '<C-r>' '<C-g>'
