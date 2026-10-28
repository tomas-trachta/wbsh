#!/bin/bash
# Tab completion reads the word at the cursor as the shell does -- quotes
# and backslashes taken out, the value after `=` completed on its own --
# and puts a file name back quoted the way the word was, or with
# backslashes before what the shell would read specially. File names
# match regardless of case (completion-ignore-case is on by default).
#
# `__lineedit` types its arguments (here <TAB> among them) into a fresh
# line editor and prints the line as [before cursor][after cursor].

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
mkdir 'a b' 'My Documents' Music
touch "it's here.txt" 'cost$1.txt' 'Readme.md' 'x&y.txt'
touch 'My Documents/report one.txt' 'My Documents/report two.txt'

echo "=== spaces and other specials get backslashes"
__lineedit 'cd a' '<TAB>'
__lineedit 'cat it' '<TAB>'
__lineedit 'cat cost' '<TAB>'
__lineedit 'cat x' '<TAB>'

echo "=== a quoted or escaped word completes inside its quotes"
__lineedit 'cd "My D' '<TAB>'
__lineedit 'cat "My Documents/report o' '<TAB>'
__lineedit "cat 'My Documents/report o" '<TAB>'
__lineedit "cat 'it" '<TAB>'
__lineedit 'cat My\ Documents/r' '<TAB>'
__lineedit 'cat My\ Documents/report\ t' '<TAB>'
__lineedit 'cat "cost' '<TAB>'

echo "=== the common part of several matches, without closing the quote"
__lineedit 'ls "M' '<TAB>'
__lineedit 'ls My' '<TAB>'

echo "=== case does not have to match, and the line takes the file's case"
__lineedit 'cat readme' '<TAB>'
__lineedit 'cd my\ d' '<TAB>'
__lineedit 'cd mu' '<TAB>'
bind 'set completion-ignore-case off'
__lineedit 'cat readme' '<TAB>'
bind 'set completion-ignore-case on'

echo "=== after = only the value is completed"
__lineedit 'make --file=Rea' '<TAB>'
__lineedit 'X=Rea' '<TAB>'
__lineedit 'P=/nonexistent:Rea' '<TAB>'

echo "=== the list shows file names without their directory"
__lineedit -o -c 80 'ls My\ Documents/rep' '<TAB>' '<TAB>' '<C-c>' | \
    grep -ao 'report one.txt *report two.txt'

cd /
rm -rf "$D"
