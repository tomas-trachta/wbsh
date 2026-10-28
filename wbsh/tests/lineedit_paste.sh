#!/bin/bash
# Pasting into the line editor: a terminal in bracketed paste mode wraps
# the text in \e[200~ ... \e[201~, and its lines become lines of one
# command that runs only when Enter is pressed.

echo "=== a bracketed paste is inserted whole and drawn once"
__lineedit -o 'echo x ' '<ESC>' '[200~echo a' '<ENTER>' 'echo b' '<ESC>' '[201~'

echo "=== Enter inside the paste is a line break; Enter after it runs the lines"
__lineedit '<ESC>' '[200~for i in 1 2' '<ENTER>' 'do echo $i; done' '<ESC>' '[201~' '<ENTER>'

echo "=== a line break at the end of the paste does not run it"
__lineedit '<ESC>' '[200~ls -l' '<ENTER>' '<ESC>' '[201~'

echo "=== keys inside the paste are text, not commands"
__lineedit 'a' '<ESC>' '[200~b' '<TAB>' 'c' '<ESC>' '[201~' 'd'

echo "=== a paste is one change for undo"
__lineedit 'a ' '<ESC>' '[200~pasted text' '<ESC>' '[201~' '<C-_>'

echo "=== a paste cut short by the end of input keeps what came"
__lineedit '<ESC>' '[200~partial'

echo "=== bind lists the paste command and its variable"
bind -q bracketed-paste-begin
bind -v | grep bracketed
