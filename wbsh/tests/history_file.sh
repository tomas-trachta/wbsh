#!/bin/bash
# The history file is appended to as each command finishes, so shells
# sharing it keep each other's entries, and HISTSIZE, HISTFILESIZE,
# HISTCONTROL and HISTIGNORE are honoured as in bash.
#
# Each session is an interactive wbsh reading its lines from a pipe, with
# HOME and HISTFILE in a scratch directory.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

D=$(mktemp -d) || exit 1

session() {
    env -u WBSH_INIT_COMMAND -u HISTSIZE -u HISTFILESIZE -u HISTCONTROL -u HISTIGNORE \
        HOME="$D" HISTFILE="$D/hist" WBSH_UNDER_TEST="$WBSH" PS1='$ ' PS2='> ' \
        "$@" "$WBSH" -i > /dev/null 2>&1
}

entries() {
    sed '1d; s/^[0-9]*\t//' "$D/hist"
}

echo "=== each command is in the file as soon as it finishes"
rm -f "$D/hist"
session <<'EOF'
echo first
cat "$HOME/hist" > "$HOME/seen"
EOF
sed 's/\t/<TAB>/' "$D/seen"

echo "=== a shell started meanwhile keeps its entries when the first one exits"
rm -f "$D/hist"
session <<'EOF'
echo a1
printf 'echo b1\necho b2\n' | env -u WBSH_INIT_COMMAND "$WBSH_UNDER_TEST" -i > /dev/null 2>&1
echo a2
EOF
entries

echo "=== the exit status of each command is kept"
rm -f "$D/hist"
session <<'EOF'
true
false
EOF
sed 's/\t/<TAB>/' "$D/hist"

echo "=== HISTFILESIZE trims the file when the shell exits"
rm -f "$D/hist"
session env HISTFILESIZE=2 <<'EOF'
echo one
echo two
echo three
EOF
entries

echo "=== HISTSIZE limits the list in memory"
session env HISTSIZE=2 <<'EOF'
echo four
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== HISTCONTROL=ignorespace and ignoredups"
rm -f "$D/hist"
session env HISTCONTROL=ignorespace:ignoredups <<'EOF'
 echo hidden
echo same
echo same
echo other
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== without HISTCONTROL duplicates and leading spaces are kept"
rm -f "$D/hist"
session <<'EOF'
 echo spaced
echo same
echo same
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== HISTCONTROL=erasedups removes the older copies"
rm -f "$D/hist"
session env HISTCONTROL=erasedups <<'EOF'
echo x
echo y
echo x
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== HISTIGNORE patterns and &"
rm -f "$D/hist"
session env 'HISTIGNORE=ls*:&' <<'EOF'
ls -d /
echo kept
echo kept
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== set +o history stops recording"
rm -f "$D/hist"
session <<'EOF'
echo before
set +o history
echo during
set -o history
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== HISTFILE set in .wbshrc is the file used"
rm -f "$D/hist" "$D/other"
echo 'HISTFILE=$HOME/other' > "$D/.wbshrc"
session <<'EOF'
echo into-other
EOF
rm -f "$D/.wbshrc"
[ -f "$D/hist" ] && echo "hist written" || echo "hist untouched"
sed '1d' "$D/other"

echo "=== an empty HISTFILE saves nothing"
rm -f "$D/hist"
session env HISTFILE= <<'EOF'
echo unsaved
EOF
[ -f "$D/hist" ] && echo "hist written" || echo "hist untouched"

echo "=== history -a appends the new entries, history -w rewrites the file"
rm -f "$D/out"
history -c
history -s 'echo appended'
HISTFILE=$D/out history -a
HISTFILE=$D/out history -a
sed 's/\t/<TAB>/' "$D/out"
history -s 'echo second'
HISTFILE=$D/out history -w
sed 's/\t/<TAB>/' "$D/out"
ls "$D" | grep -c tmp

rm -rf "$D"
