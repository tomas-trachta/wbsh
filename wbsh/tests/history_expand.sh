#!/bin/bash
# History expansion in an interactive shell, as bash does it: `!` is left
# alone inside single quotes, after a backslash, in ${!name}, [!...] and
# $!; event designators, word designators and modifiers; "event not found"
# drops the line; `set +H` turns it off. The expanded line is echoed on
# stderr before it runs.
#
# Each session is an interactive wbsh reading its lines from a pipe, with
# HOME and HISTFILE in a scratch directory; the banner is cut off.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

D=$(mktemp -d) || exit 1

session() {
    rm -f "$D/hist"
    env -u WBSH_INIT_COMMAND -u HISTCONTROL -u HISTIGNORE HOME="$D" HISTFILE="$D/hist" \
        PS1='$ ' PS2='> ' "$WBSH" -i 2>&1 | tail -n +3
}

echo "=== quoting and the contexts where ! is not an event"
session <<'EOF'
echo first
ref=x
x=val
echo ${!ref}
echo 'a!!b'
echo "a!!b"
echo "x!"
echo \!!
echo a=! b!
case x in [!y]) echo bracket;; esac
: $! && echo dollar-bang
EOF

echo "=== events"
session <<'EOF'
echo one
echo two
echo !!
echo !-3
echo !1
echo !?tw?
echo !ec
echo !#
EOF

echo "=== word designators"
session <<'EOF'
echo alpha beta gamma
echo !$ !^ !*
echo !!:2
echo !-2:1-2 !-2:0
echo !-3:2* x
echo !-4:1-
ls -d / > /dev/null; echo !$
EOF

echo "=== modifiers and quick substitution"
session <<'EOF'
echo /tmp/dir/file.tar.gz
echo !$:h !$:t !$:r !$:e
echo one two one
echo !!:s/one/1/
echo !-2:gs/one/1/
^two^2^
echo !-4:q
echo stays; echo !!:p
history 3
EOF

echo "=== errors drop the line"
session <<'EOF'
echo hi
echo !nosuchcommand
echo !99
echo !!:9
echo hi
echo !!:s/zzz/y/
history
EOF

echo "=== a continuation line inside single quotes is left alone"
session <<'EOF'
echo base
echo 'a
!!'
echo "b
!!"
EOF

echo "=== set +H turns expansion off"
session <<'EOF'
echo on
set +H
echo !!
set -H
echo !!
EOF

echo "=== a script is not history-expanded"
printf 'echo hi\necho !!\n' > "$D/script.sh"
"$WBSH" -r "$D/script.sh"

rm -rf "$D"
