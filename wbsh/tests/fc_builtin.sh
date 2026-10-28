#!/bin/bash
# The `fc` builtin: list (-l -n -r), run again (-s, -e -) and edit-then-run
# commands from the history. Commands are named by number, by a negative
# offset, or by how they start.

show() { sed 's/\t/<TAB>/'; }

history -c
history -s 'echo one'
history -s 'echo two two'
history -s 'ls -la'
history -s 'echo three'

echo "=== fc -l lists the last commands, numbered"
fc -l | show
echo "--- a range, numbers and offsets"
fc -l 2 3 | show
fc -l -2 | show
fc -l 3 | show
echo "--- by how a command starts"
fc -l ls | show
fc -l echo\ t | show
echo "--- -n leaves the numbers out, -r and first > last reverse"
fc -ln 1 2 | show
fc -lr 1 3 | show
fc -l 3 1 | show
fc -lnr | show

echo "=== errors"
fc -l 99; echo "status $?"
fc -l nosuch; echo "status $?"
fc -x; echo "status $?"
fc -e; echo "status $?"

echo "=== fc -s runs a command again, after pat=rep substitutions"
fc -s 2>&1
fc -s two=2 2 2>&1
fc -s e=E l=L 1 2>&1
fc -e - 'echo o' 2>&1
fc -s nosuch; echo "status $?"
echo "--- its status is the command's"
history -s 'false'
fc -s false; echo "status $?"

echo "=== fc runs what the editor leaves in the file"
history -c
history -s 'echo edit me'
history -s 'echo second'
FCEDIT=: fc 1 2>&1
fc -e : -1 2>&1
fc -e 'sed -i s/me/you/' 1 2>&1
fc -e : 1 2 2>&1
echo "--- an editor that fails runs nothing"
fc -e false 1; echo "status $?"
echo "--- an emptied file runs nothing"
fc -e 'truncate -s 0' 1; echo "status $?"

echo "=== in an interactive shell the fc line is replaced by what it runs"
WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
D=$(mktemp -d) || exit 1
env -u WBSH_INIT_COMMAND -u HISTCONTROL -u HISTIGNORE HOME="$D" HISTFILE="$D/hist" PS1='$ ' PS2='> ' \
    "$WBSH" -i <<'EOF' 2>&1 | grep -v '^wbsh \|type `exit`'
echo alpha
echo beta beta
fc -l
fc -s beta=gamma echo
fc -s alpha
fc -l
history 4
EOF
rm -rf "$D"
