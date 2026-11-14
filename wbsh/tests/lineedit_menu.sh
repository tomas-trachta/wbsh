#!/bin/bash
# Menu selection: with `set menu-select on` an ambiguous Tab shows the
# matches as a grid below the line and puts the selected one in the line.
# Tab / Down go to the next, Shift-Tab / Up to the previous, Left / Right
# move a column, PageUp / PageDown a screenful; Enter finishes the word,
# Esc gives the typed word back, and any other key keeps the match and
# then does what it always does. A grid taller than the screen scrolls,
# wide characters keep their columns, and descriptions sit beside the
# matches that have them. Off (the default), Tab lists as bash does.
#
# `__lineedit -o -c COLUMNS -l LINES` also prints what the editor wrote.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

complete -W "alpha alpine beta" foo
complete -W "w01 w02 w03 w04 w05 w06 w07 w08 w09 w10 w11 w12" many
complete -W "日本語 日本 中文字 abc" cjk
described() {
	local word
	for word in $'--all\tshow everything' $'--almost\tnearly all' $'--brief'; do
		[[ $word == "$2"* ]] && COMPREPLY+=("$word")
	done
}
complete -F described desc

echo "=== off by default: a second Tab lists"
__lineedit -o 'foo al' '<TAB>' '<TAB>' | tail -2 | head -1

bind 'set menu-select on'

echo "=== the grid, the first match taken at once"
__lineedit -o -c 40 -l 10 'foo al' '<TAB>' '<TAB>' '<ENTER>'

echo "=== moving and leaving"
__lineedit 'foo al' '<TAB>' '<ENTER>'
__lineedit 'foo al' '<TAB>' '<TAB>' '<ENTER>'
__lineedit 'foo al' '<TAB>' '<S-TAB>' '<ENTER>'
__lineedit 'foo al' '<TAB>' '<DOWN>' '<UP>' '<ENTER>'
__lineedit 'foo al' '<TAB>' '<ESC>'
__lineedit 'foo al' '<TAB>' '<C-g>'
__lineedit 'foo al' '<TAB>' '<SHIFT>' '<DOWN>' 'x'
__lineedit 'foo al' '<TAB>' '<DOWN>' '<BS>'
__lineedit 'foo b' '<TAB>'

echo "=== columns: Right and Left move across, wrapping a row"
__lineedit -c 16 'many w' '<TAB>' '<RIGHT>' '<ENTER>'
__lineedit -c 16 'many w' '<TAB>' '<LEFT>' '<ENTER>'
__lineedit -c 16 'many w' '<TAB>' '<PGDN>' '<ENTER>'

echo "=== a grid taller than the screen scrolls"
__lineedit -o -c 12 -l 4 'many w' '<TAB>' '<PGDN>' '<DOWN>' '<ENTER>' | tail -4

echo "=== wide characters"
__lineedit -o -c 20 -l 10 'cjk ' '<TAB>' '<RIGHT>' '<ENTER>' | tail -3

echo "=== descriptions"
__lineedit -o -c 40 -l 10 'desc --' '<TAB>' '<DOWN>' '<ENTER>' | tail -5

echo "=== command names carry their manual's summary"
saved_path=$PATH
PATH=/nonexistent
__lineedit -o -c 70 -l 10 'mk' '<TAB>' '<ENTER>' > menu.txt
PATH=$saved_path
tail -5 menu.txt

echo "=== menu-complete-display-prefix: nothing chosen until the next Tab"
bind 'set menu-complete-display-prefix on'
__lineedit 'many w' '<TAB>' '<ENTER>'
__lineedit 'many w' '<TAB>' '<TAB>' '<ENTER>'
bind 'set menu-complete-display-prefix off'

echo "=== vi insert mode"
set -o vi
__lineedit 'many w0' '<TAB>' '<TAB>' '<ENTER>'
__lineedit 'many w0' '<TAB>' '<ESC>' 'x'
set -o emacs

echo "=== the menu-select command, whatever the option says"
bind 'set menu-select off'
bind '"\e[Z": menu-select'
__lineedit 'many w1' '<S-TAB>' '<TAB>' '<ENTER>'

echo "=== menu-complete with display-prefix, without the grid"
bind '"\C-n": menu-complete'
bind 'set menu-complete-display-prefix on'
__lineedit 'foo al' '<C-n>'
__lineedit 'foo al' '<C-n>' '<C-n>'
bind 'set menu-complete-display-prefix off'
__lineedit 'foo al' '<C-n>'

cd / && rm -rf "$D"
