#!/bin/bash
# HISTTIMEFORMAT: each history entry keeps the time it was added; with the
# variable set, `history` shows it in that strftime format and the history
# file keeps it as a `#<seconds>` line before the entry, as bash does.

echo "=== without HISTTIMEFORMAT no time is shown"
history -c
history -s 'echo one'
history

echo "=== with it, the time comes before each entry in its format"
HISTTIMEFORMAT='[at] '
history
HISTTIMEFORMAT='%Y-%m-%d %H:%M:%S '
history | grep -cE '^ +1  [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} echo one$'
HISTTIMEFORMAT=
history
unset HISTTIMEFORMAT

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
D=$(mktemp -d) || exit 1

session() {
    env -u WBSH_INIT_COMMAND -u HISTSIZE -u HISTFILESIZE -u HISTCONTROL -u HISTIGNORE \
        -u HISTTIMEFORMAT HOME="$D" HISTFILE="$D/hist" PS1='$ ' PS2='> ' \
        "$@" "$WBSH" -i 2>&1 | grep -v '^\$ $' | sed 's/^\$ //'
}

echo "=== times read from the file are shown; entries without one show ??"
printf '%s\n' '#!wbsh-history-v3' '#1700000000' '0	echo stamped' '0	echo plain' > "$D/hist"
session HISTTIMEFORMAT='%Y ' <<'EOF' | grep -v '^wbsh \|type `exit`' | head -n 2
history
EOF

echo "=== with HISTTIMEFORMAT set the file gets the times"
rm -f "$D/hist"
session HISTTIMEFORMAT='%F ' <<'EOF' > /dev/null
echo first
echo second
EOF
sed -e 's/^#[0-9]\{9,\}$/#<seconds>/' "$D/hist"

echo "=== without it the file has none"
rm -f "$D/hist"
session <<'EOF' > /dev/null
echo first
EOF
cat "$D/hist"

echo "=== a shell without HISTTIMEFORMAT keeps the times when it trims the file"
printf '%s\n' '#!wbsh-history-v3' '#1700000000' '0	echo a' '#1700000001' '0	echo b' > "$D/hist"
session HISTFILESIZE=2 <<'EOF' > /dev/null
true
EOF
cat "$D/hist"

echo "=== HISTCONTROL=ignoreboth keeps a command with a leading space out"
rm -f "$D/hist"
session HISTCONTROL=ignoreboth <<'EOF' > /dev/null
echo kept
 echo secret-command --token x
echo kept
EOF
sed '1d' "$D/hist"
rm -rf "$D"
