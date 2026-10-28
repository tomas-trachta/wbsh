#!/bin/bash
# `bind` with what .bashrc and .inputrc files commonly hold: readline
# functions the line editor now has, functions and variables it does not
# know (taken without a word, as readline does), and the completion
# variables it honours.

echo "=== lines from a typical .bashrc load without errors"
bind '"\e[A": history-search-backward'; echo "status $?"
bind '"\e[B": history-search-forward'; echo "status $?"
bind 'set completion-ignore-case on'; echo "status $?"
bind 'set show-all-if-ambiguous on'; echo "status $?"
bind 'set colored-stats on'; echo "status $?"
bind 'set mark-symlinked-directories on'; echo "status $?"
bind '"\C-xq": some-function-wbsh-lacks'; echo "status $?"
bind 'TAB: menu-complete'; echo "status $?"
bind '"\e[Z": menu-complete-backward'; echo "status $?"
bind -p | head -1
bind 'set show-all-if-ambiguous off'
bind 'TAB: complete'
bind -r '\e[Z'

echo "=== unknown function names are refused only where bash refuses them"
bind -q some-function-wbsh-lacks; echo "status $?"
bind -u some-function-wbsh-lacks; echo "status $?"

echo "=== history-search-backward / -forward match the text before the cursor"
history -c
history -s 'git status'
history -s 'ls -la'
history -s 'git commit -m one'
history -s 'echo git'
__lineedit 'git' '<UP>'
__lineedit 'git' '<UP>' '<UP>'
__lineedit 'git' '<UP>' '<UP>' '<UP>'
__lineedit 'git' '<UP>' '<UP>' '<DOWN>'
__lineedit 'zzz' '<UP>'

echo "=== history-substring-search finds the text anywhere"
bind '"\e[A": history-substring-search-backward'
__lineedit 'git' '<UP>'
__lineedit 'git' '<UP>' '<UP>'
bind '"\e[A": previous-history'
bind '"\e[B": next-history'

echo "=== menu-complete steps through the matches, then back to the word"
d=$(mktemp -d)
: > "$d/alpha1"; : > "$d/alpha2"; : > "$d/beta"
cd "$d" || exit 1
bind '"\C-xm": menu-complete'
bind '"\C-xn": menu-complete-backward'
__lineedit 'cat al' '<C-x>' 'm'
__lineedit 'cat al' '<C-x>' 'm' '<C-x>' 'm'
__lineedit 'cat al' '<C-x>' 'm' '<C-x>' 'm' '<C-x>' 'm'
__lineedit 'cat al' '<C-x>' 'n'
__lineedit 'cat be' '<C-x>' 'm'

echo "=== insert-completions (M-*) puts every match in"
__lineedit 'cat al' '<M-*>'

echo "=== complete-filename (M-/), complete-command (M-!), complete-variable (M-\$)"
__lineedit 'echo al' '<M-/>'
__lineedit 'histor' '<M-!>'
zzvariable_one=1
__lineedit 'echo $zzvar' '<M-$>'
__lineedit 'echo ${zzvar' '<M-$>'
cd - > /dev/null
rm -rf "$d"

echo "=== the mark and the region"
__lineedit 'one two three' '<C-a>' '<M-f>' '<C-@>' '<C-e>' '<C-x>' '<C-x>' 'X'
bind '"\C-xk": kill-region'
bind '"\C-xc": copy-region-as-kill'
__lineedit 'one two three' '<C-a>' '<M-f>' '<C-@>' '<M-f>' '<C-x>' 'k'
__lineedit 'one two three' '<C-a>' '<C-@>' '<M-f>' '<C-x>' 'c' '<C-e>' '<C-y>'

echo "=== copy-backward-word, copy-forward-word, backward-kill-line, unix-filename-rubout"
bind '"\C-xb": copy-backward-word'
bind '"\C-xf": copy-forward-word'
bind '"\C-xr": unix-filename-rubout'
__lineedit 'one two' '<C-x>' 'b' ' ' '<C-y>'
__lineedit 'one two' '<C-a>' '<C-x>' 'f' '<C-e>' ' ' '<C-y>'
__lineedit 'cd /usr/local/bin' '<LEFT>' '<LEFT>' '<C-x>' '<BS>'
__lineedit 'cd /usr/local/bin' '<C-x>' 'r'
__lineedit 'cd /usr/local/bin/' '<C-x>' 'r' '<C-x>' 'r'

echo "=== character-search (C-]) and character-search-backward (M-C-])"
__lineedit 'find . -name x' '<C-a>' '<C-]>' 'n' 'X'
__lineedit 'find . -name x' '<C-a>' '<C-]>' 'q' 'X'
__lineedit 'find . -name x' '<M-C-]>' '.' 'X'

echo "=== quoted-insert (C-q) types the next key as it is"
__lineedit 'a' '<C-q>' '<C-a>' 'b'
__lineedit 'a' '<C-q>' '<TAB>' 'b'

echo "=== tab-insert (M-TAB), overwrite-mode"
__lineedit 'a' '<M-TAB>' 'b'
bind '"\C-xo": overwrite-mode'
__lineedit 'hello' '<C-a>' '<C-x>' 'o' 'J' 'é'
__lineedit 'hi' '<C-a>' '<C-x>' 'o' 'abcd'
__lineedit 'hello' '<C-a>' '<C-x>' 'o' '<C-x>' 'o' 'J'

echo "=== delete-char-or-list, forward-backward-delete-char"
bind '"\C-xd": delete-char-or-list'
bind '"\C-xe": forward-backward-delete-char'
__lineedit 'abc' '<C-a>' '<C-x>' 'd'
__lineedit 'abc' '<C-a>' '<C-x>' 'e'
__lineedit 'abc' '<C-x>' 'e'

echo "=== operate-and-get-next (C-o) runs the line and brings up the next entry"
history -c
history -s 'echo first'
history -s 'echo second'
history -s 'echo third'
__lineedit '<UP>' '<UP>' '<UP>' '<C-o>'
__lineedit '<UP>' '<C-o>'
__lineedit 'echo new' '<C-o>'

echo "=== a second Tab lists an ambiguous word unless show-all-if-unmodified is on"
d=$(mktemp -d)
: > "$d/same1"; : > "$d/same2"
cd "$d" || exit 1
__lineedit -o -c 80 'ls same' '<TAB>' | grep -c 'same1  same2'
bind 'set show-all-if-unmodified off'
__lineedit -o -c 80 'ls same' '<TAB>' | grep -c 'same1  same2'
__lineedit -o -c 80 'ls same' '<TAB>' '<TAB>' | grep -c 'same1  same2'
__lineedit -o -c 80 'ls s' '<TAB>' | grep -c 'same1  same2'
bind 'set show-all-if-ambiguous on'
__lineedit -o -c 80 'ls s' '<TAB>' | grep -c 'same1  same2'
__lineedit 'ls s' '<TAB>'
bind 'set show-all-if-ambiguous off'
bind 'set show-all-if-unmodified on'
cd - > /dev/null
rm -rf "$d"
