#!/bin/bash
# The line editor and multi-line prompts, \[ \] and escape sequences in
# them: only the prompt's last line is drawn again as the line changes,
# and the cursor is placed by the cells the prompt really takes.
#
# `__lineedit -p PROMPT -c COLUMNS -o KEYS...` types KEYS after PROMPT on
# a screen COLUMNS wide and prints the line as [before][after] @row:col
# (the cursor's cell, counted from the prompt's last line), then what the
# editor wrote with its escapes spelled out. PS1's \[ and \] reach the
# editor as \001 and \002.

echo "=== a two-line prompt: only its last line is redrawn"
__lineedit -p $'user@host ~\n$ ' -c 20 -o 'ab' '<LEFT>'

echo "=== a three-line prompt"
__lineedit -p $'one\ntwo\n> ' -c 20 -o 'x'

echo "=== \\[ \\] around a window-title OSC and colours take no cells"
__lineedit -p $'\001\e]0;~/src\a\002$ ' -c 80 'abc'
__lineedit -p $'\001\e[32;1m\002user\001\e[0m\002 $ ' -c 80 'abc'

echo "=== escape sequences and a bell take none even without \\[ \\]"
__lineedit -p $'\e]0;title\a$ ' -c 80 'abc'
__lineedit -p $'\e[1mbold\e[0m $ ' -c 80 'abc'
__lineedit -p $'\a$ ' -c 80 'abc'

echo "=== a newline inside \\[ \\] does not start a line"
__lineedit -p $'\001\n\002$ ' -c 20 'a'

echo "=== a long last line wraps, and the cursor follows"
__lineedit -p 'abcdefghijkl$ ' -c 10 -o 'x'
__lineedit -p 'abcdefghij' -c 10 'x'
__lineedit -p $'top\nabcdefghij' -c 10 -o 'x'

echo "=== the REPL turns \\[ \\] into the marks and prints the prompt without them"
D=$(mktemp -d) || exit 1
printf 'echo ran\n' | env -u WBSH_INIT_COMMAND HOME="$D" HISTFILE="$D/hist" \
    PS1='\[\e[1m\]B\[\e[0m\]\n\$ ' ../../x64/Release/wbsh.exe -i 2>&1 | tail -n +3 | od -c | sed 's/  */ /g'
rm -rf "$D"
