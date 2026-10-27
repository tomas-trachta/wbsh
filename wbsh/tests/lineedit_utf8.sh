#!/bin/bash
# Line editor: cursor movement and erasing step over whole characters.
#
# `__lineedit` types its arguments into a fresh line editor (literal text,
# or a key such as <LEFT> <BS> <DEL>) and prints the line as
# [before cursor][after cursor].

echo "=== Backspace erases a whole two-byte character"
__lineedit 'echo č' '<BS>'
__lineedit 'žluť' '<BS>' '<BS>'

echo "=== Left then typing lands between characters"
__lineedit 'ač' '<LEFT>' 'x'
__lineedit 'řeka' '<HOME>' '<RIGHT>' 'x'
__lineedit 'abč' '<C-b>' '<C-b>' '<C-f>' 'x'

echo "=== Delete erases the character under the cursor"
__lineedit 'ačb' '<LEFT>' '<LEFT>' '<DEL>'
__lineedit 'ačb' '<HOME>' '<RIGHT>' '<C-d>'

echo "=== three- and four-byte characters"
__lineedit 'a€b' '<LEFT>' '<BS>'
__lineedit 'x😀y' '<LEFT>' '<LEFT>' 'z'
__lineedit '漢字' '<BS>'

echo "=== a letter with a combining accent moves and erases as one"
__lineedit "$(printf 'e\xcc\x81x')" '<LEFT>' '<LEFT>' 'a'
__lineedit "$(printf 'oe\xcc\x81')" '<BS>'

echo "=== an emoji with a skin-tone modifier moves and erases as one"
__lineedit "$(printf 'a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd')" '<BS>'
__lineedit "$(printf '\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbdz')" '<LEFT>' '<LEFT>' 'y'

echo "=== ASCII is unchanged"
__lineedit 'hello' '<LEFT>' '<LEFT>' '<BS>' 'L'
__lineedit 'abc' '<HOME>' '<DEL>' '<END>' 'd'
__lineedit 'abc' '<ENTER>'
__lineedit '<C-d>'
