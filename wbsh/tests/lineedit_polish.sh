#!/bin/bash
# Line editor details: a resize while editing, the question before a long
# completion list, aliases and keywords among command names, and CRLF
# input to an interactive shell.

echo "=== a resize lays the line out again at the new width"
__lineedit -c 20 -p '$ ' 'echo this line is long' '<RESIZE:10>'
__lineedit -c 20 -o -p '$ ' 'echo this line is long' '<RESIZE:10>' 'X' | tail -n 2
__lineedit -c 10 -o -p '$ ' 'echo this line is long' '<RESIZE:40>' | tail -n 2
__lineedit -c 20 -p '$ ' 'echo hi' '<RESIZE:33>'
echo "COLUMNS=$COLUMNS"

echo "=== a list of completion-query-items matches or more asks first"
d=$(mktemp -d)
for i in $(seq 1 120); do : > "$d/file$i"; done
cd "$d" || exit 1
__lineedit -o -c 80 'ls f' '<TAB>' '<TAB>' 'n' | grep -o 'Display all [0-9]* possibilities? (y or n)'
__lineedit -o -c 80 'ls f' '<TAB>' '<TAB>' 'y' | grep -c 'file1  '
__lineedit -o -c 80 'ls f' '<TAB>' '<TAB>' 'q' 'n' | grep -c 'possibilities? (y or n)\\a'
__lineedit -c 80 'ls f' '<TAB>' '<TAB>' 'n' 'x'
bind 'set completion-query-items 0'
__lineedit -o -c 80 'ls f' '<TAB>' '<TAB>' | grep -c 'Display all'
bind 'set completion-query-items 200'
__lineedit -o -c 80 'ls f' '<TAB>' '<TAB>' | grep -c 'Display all'
bind 'set completion-query-items 100'
cd - > /dev/null
rm -rf "$d"

echo "=== list columns are padded by cells"
d=$(mktemp -d)
: > "$d/žluť.txt"; : > "$d/žába.txt"; : > "$d/zz.txt"
__lineedit -o -c 80 "ls $d/" '<TAB>' '<TAB>' | grep 'txt' | sed 's/\\r\\n$//'
rm -rf "$d"

echo "=== command names include aliases and keywords"
alias zzmyalias='echo hi'
__lineedit 'zzmya' '<TAB>'
__lineedit 'whil' '<TAB>'
__lineedit 'func' '<TAB>'
__lineedit 'sel' '<TAB>'

echo "=== CRLF lines piped into an interactive shell lose the CR"
WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
H=$(mktemp -d)
printf 'echo done\r\nx=abc\r\necho "[$x]"\r\n\r\necho last' \
    | env -u WBSH_INIT_COMMAND HOME="$H" HISTFILE="$H/hist" PS1='$ ' "$WBSH" -i 2>/dev/null \
    | grep -a -E 'done|abc|last' | sed 's/^[$ ]*//' | od -c
rm -rf "$H"
