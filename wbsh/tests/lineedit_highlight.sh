#!/bin/bash
# Syntax highlighting of the line being typed. `__lineedit -H -o KEYS...`
# colours the line as an interactive shell does and prints what the
# editor wrote; `frame` keeps the last drawing of the line, escapes
# spelled out. The last part types into a real shell on a pseudoconsole.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

T=_lineedit_highlight_tmp
rm -rf $T
mkdir -p $T/home
touch $T/file.txt

frame() {
	__lineedit -H -o "$@" | tail -n 1 | sed -e 's/.*\\r\\e\[J//' -e 's/\\r\\e\[[0-9]*C$//'
}

unset NO_COLOR WBSH_HIGHLIGHT
TERM=xterm-256color

echo "=== commands, unknown words, strings, expansions, operators, options, comments"
frame 'echo "hi $USER" | nosuchcmd-xyz --long -x > out 2>&1 # note'
frame "printf '%s\\n' \$'a\\tb' \${HOME} \$(true) \`pwd\` \$((1 + 2))"

echo "=== keywords, loop names, case patterns, [[ ]], assignments"
frame 'if true; then :; elif false; then :; else :; fi'
frame 'for i in a b; do echo $i; done'
frame 'case $x in a|b) echo y;; esac'
frame '[[ -n $a && $a == b ]] && echo ok'
frame 'X=1 Y+=2 env -i A=b cmd-xyz'
frame 'a=(1 2 3); f() { :; }; f'
frame '! true; !!; time command -v nosuchcmd-xyz'
defined() { :; }
alias aliased=true
frame 'defined; aliased; \aliased'

echo "=== a path that exists is underlined"
frame "cat $T/file.txt $T/missing.txt"
frame "./$T/file.txt"

echo "=== unfinished quotes and substitutions colour to the end"
frame "echo 'unterminated"
frame 'echo $(ls | nosuchcmd-xyz'

echo "=== the lines of a paste are coloured as one command"
__lineedit -H -o '<ESC>' '[200~if true' '<ENTER>' "then echo 'a" '<ENTER>' "b'; fi" '<ESC>' '[201~' | tail -n 3

echo "=== here-documents and descriptors"
frame 'cat <<EOF >&2'

echo "=== wide characters keep the cursor where it was"
__lineedit -H -c 20 'echo 日本語' '<LEFT>'
__lineedit -c 20 'echo 日本語' '<LEFT>'

echo "=== the ghost text of a prediction follows the colours"
history -c
history -s 'echo prediction here'
frame 'echo pre'

echo "=== reverse search colours the match"
__lineedit -H -o '<C-r>' 'here' | tail -n 1 | sed -e 's/\\r\\e\[42C.*//' -e 's/.*\\r\\e\[J//'

echo "=== vi command mode keeps the colours"
bind 'set editing-mode vi'
frame 'echo hi' '<ESC>' 'b'
bind 'set editing-mode emacs'

echo "=== NO_COLOR, TERM=dumb and syntax-highlight off leave the line plain"
NO_COLOR=1 frame 'echo hi | nosuchcmd-xyz'
TERM=dumb frame 'echo hi | nosuchcmd-xyz'
bind 'set syntax-highlight off'
frame 'echo hi | nosuchcmd-xyz'
bind 'set syntax-highlight on'

echo "=== colours from inputrc, and WBSH_HIGHLIGHT over them"
bind 'set syntax-highlight-colors command=1;32:keyword=34'
frame 'if echo; then :; fi'
WBSH_HIGHLIGHT='keyword=0:unknown=4;31:bogus=1:string=x' frame "if nosuchcmd-xyz 'a'; then :; fi"
bind 'set syntax-highlight-colors ""'
bind -v | grep syntax-highlight

echo "=== a replay without -H stays plain"
__lineedit -o 'echo hi' | tail -n 1

echo "=== typed on a pseudoconsole"
export HOME=$PWD/$T/home INPUTRC=/dev/null PS1='$ '
typed() {
	{ sleep 2; printf '%s' "$1"; sleep 1; printf '\r'; sleep 1; printf '%s' "$2"; sleep 1; printf '\003'
	  sleep 1; printf 'exit\r'; sleep 1; } | unbuffer -p $W --norc | tr -d '\r' | canonical_sgr
}

# The in-box conhost re-renders `\e[0;32m...\e[0m` as `\e[32m...\e[m`; the
# bundled ConPTY passes it through. Compare in the in-box form.
canonical_sgr() {
	sed -e $'s/\e\\[0;/\e[/g' -e $'s/\e\\[0m/\e[m/g'
}

shows() {
	grep -q "$2" <<< "$1" && echo "shows $3" || echo "does not show $3"
}

screen=$(typed "echo 'one" "two' | nosuchcmd-xyz")
shows "$screen" $'\e\\[32mecho\e\\[m \e\\[33m\'one' "a command and a string"
shows "$screen" $'\e\\[33mtwo\'\e\\[m | \e\\[31mnosuchcmd-xyz' "the string the line goes on with"
screen=$(NO_COLOR=1 typed "echo 'one" "two' | nosuchcmd-xyz")
shows "$screen" "two' | nosuchcmd-xyz" "the line under NO_COLOR"
shows "$screen" $'\e\\[31mnosuchcmd-xyz' "colours under NO_COLOR"

rm -rf $T
