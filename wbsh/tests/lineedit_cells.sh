#!/bin/bash
# The line editor places the cursor by screen cells: wide (CJK, emoji)
# characters take two, combining marks none, and a wide character that
# does not fit at the end of a row moves whole to the next one.
#
# `__lineedit -p PROMPT -c COLUMNS KEYS...` reports the line as
# [before cursor][after cursor] @row:col -- the cell the cursor is left
# in, counted from the prompt's row.

echo "=== wide characters take two cells"
__lineedit -p '$ ' -c 80 'echo 日本語日本語'
__lineedit -p '$ ' -c 80 'echo 日本語日本語' '<LEFT>' '<LEFT>'
__lineedit -p '$ ' -c 80 '😀😀' '<LEFT>'

echo "=== combining marks take none"
__lineedit -p '$ ' -c 80 "$(printf 'e\xcc\x81e\xcc\x81e\xcc\x81')"
__lineedit -p '$ ' -c 80 "$(printf 'e\xcc\x81e\xcc\x81e\xcc\x81')" '<LEFT>'

echo "=== a wide prompt counts its cells too"
__lineedit -p '日本 $ ' -c 80 'ab'

echo "=== wrapping: a wide character that does not fit goes to the next row"
__lineedit -p '$ ' -c 10 'abcdefg'
__lineedit -p '$ ' -c 10 'abcdefgh'
__lineedit -p '$ ' -c 10 'abcdefg日'
__lineedit -p '$ ' -c 10 'abcdefg日' '<LEFT>'
__lineedit -p '$ ' -c 10 'abcdef日'
__lineedit -p '$ ' -c 10 '日本語日本語日本語' '<HOME>' '<C-f>' '<C-f>' '<C-f>' '<C-f>'

echo "=== the redraw moves up exactly the rows it went down"
__lineedit -p '$ ' -c 10 -o 'abcdefg日' '<LEFT>' '<LEFT>'

echo "=== reverse search takes non-ASCII text"
history -c
history -s 'echo žluťoučký kůň'
history -s 'ls'
__lineedit '<C-r>' 'kůň' '<ENTER>'
__lineedit '<C-r>' 'kůx' '<BS>' '<BS>' '<ENTER>'
