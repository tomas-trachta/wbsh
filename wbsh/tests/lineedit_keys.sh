#!/bin/bash
# readline's emacs keys in the line editor, the kill ring, undo, and key
# bindings through `bind` and inputrc files.
#
# `__lineedit` types its arguments into a fresh line editor (literal text,
# or a key such as <C-t> <M-.> <C-LEFT> <AltGr-@>) and prints the line as
# [before cursor][after cursor], or what ended it.

echo "=== Meta keys nobody bound insert nothing; ESC works as Meta"
__lineedit 'echo hello' '<M-z>'
__lineedit 'echo hello' '<M-.>'
__lineedit 'one two' '<ESC>' 'b' 'X'
__lineedit 'abc' '<C-o>'

echo "=== AltGr characters are typed, not taken for Ctrl+Alt chords"
__lineedit 'mail ' '<AltGr-@>' 'x' '<AltGr-{>'
__lineedit 'a' '<AltGr-€>'

echo "=== Ctrl with the arrows, Backspace and Delete works on words"
__lineedit 'git commit --amend' '<C-LEFT>' 'X'
__lineedit 'git commit --amend' '<HOME>' '<C-RIGHT>' 'X'
__lineedit 'cd /usr/local/bin' '<C-BS>'
__lineedit 'git commit --amend' '<HOME>' '<C-DEL>'
__lineedit 'one two' '<M-LEFT>' 'X'

echo "=== kills go to the kill ring and Ctrl-Y yanks them back"
__lineedit 'hello world' '<C-w>' '<C-y>' '<C-y>'
__lineedit 'one two three' '<C-w>' '<C-w>' 'X' '<C-y>'
__lineedit 'abc def' '<HOME>' '<C-k>' 'x' '<C-y>'
__lineedit 'abc def' '<C-u>' 'new ' '<C-y>'

echo "=== Alt-Y swaps the yank for older kills"
__lineedit 'first second' '<C-w>' '<C-a>' '<C-k>' '<C-y>' '<M-y>'
__lineedit 'first second' '<C-w>' '<C-a>' '<C-k>' '<C-y>' '<M-y>' '<M-y>'
__lineedit 'nothing yanked' '<M-y>'

echo "=== Ctrl-T swaps characters, Alt-T words"
__lineedit 'abc' '<C-t>'
__lineedit 'abc' '<HOME>' '<RIGHT>' '<C-t>'
__lineedit 'žluť' '<C-t>'
__lineedit 'one two' '<M-t>'
__lineedit 'one two three' '<C-a>' '<M-f>' '<M-t>'

echo "=== Alt-U, Alt-L and Alt-C change the case of a word"
__lineedit 'hello world' '<HOME>' '<M-u>'
__lineedit 'HELLO WORLD' '<HOME>' '<M-f>' '<M-l>'
__lineedit 'žluťoučký kůň' '<HOME>' '<M-c>' '<M-u>'

echo "=== undo: Ctrl-_, Ctrl-X Ctrl-U and Alt-R"
__lineedit 'abc' '<C-_>'
__lineedit 'abc def' '<C-w>' '<C-_>'
__lineedit 'abc def' '<C-w>' 'xyz' '<C-_>'
__lineedit 'abc def' '<C-w>' 'xyz' '<C-_>' '<C-_>'
__lineedit 'abc' '<C-x>' '<C-u>'
__lineedit 'abc' '<C-w>' 'de' '<BS>' '<M-r>'
__lineedit '<C-_>'

echo "=== Alt-. and Alt-_ insert the last word of older commands"
history -c
history -s 'ls /tmp/foo'
history -s 'echo "a b" c'
__lineedit 'cat ' '<M-.>'
__lineedit 'cat ' '<M-.>' '<M-.>'
__lineedit 'cat ' '<M-_>'
__lineedit 'cat ' '<M-.>' '<M-.>' '<M-.>'
__lineedit 'cat ' '<C-M-y>'

echo "=== other readline commands"
__lineedit 'a   b' '<LEFT>' '<M-\>'
__lineedit 'echo hi' '<M-#>'
__lineedit 'one' '<M-<>'
__lineedit 'one' '<M-<>' '<M->>'

echo "=== Ctrl-X Ctrl-E runs the line through \$EDITOR"
export EDITOR='sed -i s/hello/bye/'
__lineedit 'echo hello' '<C-x>' '<C-e>'
unset EDITOR

echo "=== bind: functions, macros and shell commands"
bind '"\C-t": backward-char'
__lineedit 'abc' '<C-t>' 'X'
bind '"\ez": "macro text"'
__lineedit '<M-z>'
bind 'Control-o: "zz"'
__lineedit '<C-o>'
bind -x '"\C-g": READLINE_LINE="[$READLINE_LINE]"; READLINE_POINT=2'
__lineedit 'abc' '<C-g>' 'X'
bind -m emacs-ctlx '"t": transpose-words'
__lineedit 'one two' '<C-x>' 't'
bind '"\C-xy": nosuchfunction'
echo "status $?"

echo "=== bind lists and queries"
bind -q backward-char
bind -q kill-whole-line
bind -p | grep -E '"\\C-t"|"\\eb"|kill-whole-line'
bind -P | grep '^yank-pop'
bind -s
bind -X
bind -l | grep -c .
bind -v | grep bell-style

echo "=== bind -r and bind -u remove bindings"
bind -r '"\C-t"'
__lineedit 'abc' '<C-t>' 'X'
bind -u backward-word
bind -q backward-word

echo "=== bind -f reads an inputrc"
D=$(mktemp -d) || exit 1
cat > "$D/inputrc" <<'EOF'
# a comment
set bell-style none
$if mode=emacs
"\C-t": transpose-chars
$if term=nosuchterm
"\C-t": end-of-line
$endif
$endif
$if mode=vi
"\C-t": beginning-of-line
$else
Meta-q: upcase-word
$endif
EOF
bind -f "$D/inputrc"
__lineedit 'abc' '<C-t>'
__lineedit 'abc' '<HOME>' '<M-q>'
bind -v | grep bell-style

echo "=== an interactive shell reads INPUTRC before .wbshrc"
printf '"\\C-t": kill-whole-line\n' > "$D/rc"
echo 'bind "\"\\C-o\": end-of-line"' > "$D/.wbshrc"
printf 'bind -p | grep -e kill-whole-line -e end-of-line > "$HOME/out"\n' | \
    env -u WBSH_INIT_COMMAND HOME="$D" HISTFILE="$D/hist" INPUTRC="$D/rc" PS1='$ ' \
    ../../x64/Release/wbsh.exe -i > /dev/null 2>&1
cat "$D/out"
rm -rf "$D"
