#!/bin/bash
# vi editing mode: `set -o vi` / `set -o emacs`, readline's editing-mode,
# the vi keymaps for `bind -m`, and the editor's insert and command modes,
# counts, operators with motions, and `.`.
#
# `__lineedit` types its arguments into a fresh line editor and prints the
# line as [before cursor][after cursor]; each line starts in insert mode.

echo "=== set -o vi and set -o emacs switch the editing mode"
set -o | grep -E '^(emacs|vi) '
set -o vi; echo "status $?"
set -o | grep -E '^(emacs|vi) '
bind -v | grep editing-mode
set -o emacs
set -o | grep -E '^(emacs|vi) '
bind -v | grep editing-mode
bind 'set editing-mode vi'
set -o | grep -E '^(emacs|vi) '
set +o vi
set -o | grep -E '^(emacs|vi) '
bind -v | grep editing-mode

set -o vi

echo "=== insert mode types; Esc goes to command mode, one character back"
__lineedit 'hello'
__lineedit 'hello' '<ESC>'
__lineedit 'hello' '<ESC>' 'z'

echo "=== h l 0 ^ \$ w b e W B E move"
__lineedit 'one two three' '<ESC>' '0'
__lineedit 'one two three' '<ESC>' '0' 'w'
__lineedit 'one two three' '<ESC>' '0' '2w'
__lineedit 'one two three' '<ESC>' 'b'
__lineedit 'one two three' '<ESC>' '0' 'e'
__lineedit 'one two three' '<ESC>' '0' 'e' 'e'
__lineedit 'a.b c' '<ESC>' '0' 'w'
__lineedit 'a.b c' '<ESC>' '0' 'W'
__lineedit 'a.b c' '<ESC>' 'B'
__lineedit 'a.b c' '<ESC>' '0' 'E'
__lineedit '  indented' '<ESC>' '^'
__lineedit 'abc' '<ESC>' '0' 'l' 'l' 'l' 'l'
__lineedit 'abc' '<ESC>' 'h' 'h' 'h' 'h'
__lineedit 'abcdef' '<ESC>' '0' '$'
__lineedit 'abcdef' '<ESC>' '4|'
__lineedit 'f(a, (b))' '<ESC>' '0' '%'

echo "=== f F t T ; , find a character"
__lineedit 'a-b-c-d' '<ESC>' '0' 'f' '-'
__lineedit 'a-b-c-d' '<ESC>' '0' 'f' '-' ';' ';'
__lineedit 'a-b-c-d' '<ESC>' '0' '2f' '-'
__lineedit 'a-b-c-d' '<ESC>' 'F' '-'
__lineedit 'a-b-c-d' '<ESC>' '0' 't' 'c'
__lineedit 'a-b-c-d' '<ESC>' 'T' 'b'
__lineedit 'a-b-c-d' '<ESC>' '0' 'f' '-' ';' ','
__lineedit 'žluť-kůň' '<ESC>' '0' 'f' 'ů'

echo "=== i a I A R insert; x X r ~ s S edit"
__lineedit 'world' '<ESC>' '0' 'i' 'hello ' '<ESC>'
__lineedit 'ac' '<ESC>' '0' 'a' 'b'
__lineedit 'bc' '<ESC>' 'I' 'a'
__lineedit 'ab' '<ESC>' '0' 'A' 'c'
__lineedit 'abcd' '<ESC>' '0' 'R' 'XY'
__lineedit 'abcd' '<ESC>' '0' 'x'
__lineedit 'abcd' '<ESC>' '0' '2x'
__lineedit 'abcd' '<ESC>' 'X'
__lineedit 'abcd' '<ESC>' '0' 'r' 'z'
__lineedit 'abcd' '<ESC>' '0' '3r' 'z'
__lineedit 'abcd' '<ESC>' '0' '9r' 'z'
__lineedit 'abcd' '<ESC>' '0' '~' '~'
__lineedit 'žluť' '<ESC>' '0' '4~'
__lineedit 'abcd' '<ESC>' '0' 's' 'X'
__lineedit 'abcd' '<ESC>' '0' 'S' 'new'

echo "=== d c y with motions; dd cc yy; D C Y; p P"
__lineedit 'one two three' '<ESC>' '0' 'd' 'w'
__lineedit 'one two three' '<ESC>' '0' 'd' '2w'
__lineedit 'one two three' '<ESC>' '0' '2d' 'w'
__lineedit 'one two three' '<ESC>' '0' 'd' 'e'
__lineedit 'one two three' '<ESC>' 'd' 'b'
__lineedit 'one two three' '<ESC>' '0' 'w' 'd' '$'
__lineedit 'one two three' '<ESC>' '0' 'w' 'd' '0'
__lineedit 'one two three' '<ESC>' '0' 'd' 'f' 't'
__lineedit 'one two three' '<ESC>' '0' 'd' 't' 't'
__lineedit 'one two three' '<ESC>' 'd' 'F' 't'
__lineedit 'one two three' '<ESC>' 'd' 'd'
__lineedit 'one two three' '<ESC>' '0' 'w' 'D'
__lineedit 'one two three' '<ESC>' '0' 'c' 'w' 'ONE'
__lineedit 'one two three' '<ESC>' '0' 'c' '2w' 'X'
__lineedit 'a b' '<ESC>' '0' 'c' 'w' 'X'
__lineedit 'one two three' '<ESC>' '0' 'w' 'C' 'end'
__lineedit 'one two three' '<ESC>' 'c' 'c' 'new'
__lineedit 'one two' '<ESC>' '0' 'y' 'w' '$' 'p'
__lineedit 'one two' '<ESC>' '0' 'y' 'y' 'P'
__lineedit 'one two' '<ESC>' '0' 'w' 'Y' '0' 'P'
__lineedit 'abc' '<ESC>' '0' 'x' 'p'
__lineedit 'abc' '<ESC>' '0' 'x' '2p'
__lineedit 'one two' '<ESC>' 'd' 'z'

echo "=== . repeats the last change"
__lineedit 'a b c d' '<ESC>' '0' 'x' '.' '.'
__lineedit 'one two three four' '<ESC>' '0' 'd' 'w' '.'
__lineedit 'x y' '<ESC>' '0' 'i' 'Q' '<ESC>' 'w' '.'
__lineedit 'one two three' '<ESC>' '0' 'c' 'w' 'X' '<ESC>' 'w' '.'
__lineedit 'aaaa' '<ESC>' '0' 'x' 'l' '2.'

echo "=== u undoes, U goes back to the start"
__lineedit 'one two' '<ESC>' '0' 'd' 'w' 'u'
__lineedit 'one two' '<ESC>' '0' 'x' 'x' 'U'

echo "=== marks"
__lineedit 'one two three' '<ESC>' '0' 'w' 'm' 'a' '$' '`' 'a' 'x'
__lineedit 'one two three' '<ESC>' '0' 'w' 'm' 'a' '$' 'd' '`' 'a'

echo "=== history: k j G and / ? n N"
history -c
history -s 'echo first'
history -s 'ls -la'
history -s 'echo third'
__lineedit '<ESC>' 'k'
__lineedit '<ESC>' 'k' 'k'
__lineedit '<ESC>' 'k' 'k' 'j'
__lineedit '<ESC>' 'G'
__lineedit '<ESC>' '2G'
__lineedit '<ESC>' '/' 'ls' '<ENTER>'
__lineedit '<ESC>' '/' 'echo' '<ENTER>' 'n'
__lineedit '<ESC>' '/' 'echo' '<ENTER>' 'n' 'N'
__lineedit 'typed' '<ESC>' '/' 'zzz' '<ENTER>'
__lineedit 'typed' '<ESC>' '/' 'x' '<ESC>'

echo "=== Enter runs the line from either mode; Ctrl-D on an empty line ends the input"
__lineedit 'echo hi' '<ENTER>'
__lineedit 'echo hi' '<ESC>' '<ENTER>'
__lineedit '<C-d>'
__lineedit 'a' '<C-d>'

echo "=== insert-mode keys"
__lineedit 'one two' '<C-w>'
__lineedit 'one two' '<C-u>'
__lineedit 'one two' '<BS>'
__lineedit 'abc' '<LEFT>' '<LEFT>' 'X'
__lineedit 'one two' '<M-b>' 'X'

echo "=== bind and inputrc reach the vi keymaps"
bind -m vi-command '"z": vi-delete'
__lineedit 'abc' '<ESC>' '0' 'z'
bind -m vi-insert '"\C-a": beginning-of-line'
__lineedit 'abc' '<C-a>' 'X'
bind -m vi -q vi-put
bind -p | grep -c 'vi-movement-mode'
d=$(mktemp -d)
printf '%s\n' 'set keymap vi-command' '"Q": vi-rubout' '$if mode=vi' '"q": end-of-line' '$endif' > "$d/inputrc"
bind -f "$d/inputrc"
__lineedit 'abc' '<ESC>' '0' 'q' 'Q'
rm -rf "$d"

echo "=== Ctrl-E in command mode goes back to emacs keys"
__lineedit 'abc' '<ESC>' '<C-e>'
bind -v | grep editing-mode
set -o vi
__lineedit 'abc' '<M-C-j>'
set -o emacs
__lineedit 'abc' '<M-C-j>' '<ESC>' '0' 'x'
bind -v | grep editing-mode
set -o emacs
