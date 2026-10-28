#!/bin/bash
# A command typed over several lines is one history entry (bash's cmdhist):
# the lines are joined with "; ", a space, or -- inside quotes, here-docs and
# after a comment -- a newline, and the history file keeps the newlines.
#
# Each session is an interactive wbsh reading its lines from a pipe, with
# HOME and HISTFILE in a scratch directory.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

D=$(mktemp -d) || exit 1

session() {
    env -u WBSH_INIT_COMMAND HOME="$D" HISTFILE="$D/hist" PS1='$ ' PS2='> ' "$WBSH" -i \
        > "$D/screen" 2>&1
}

echo "=== loops, functions, pipes and case statements become one entry"
rm -f "$D/hist"
session <<'EOF'
for i in 1 2
do
echo $i
done
f() {
echo x
}
if true
then echo y
fi
echo 1 |
cat
case x in
x) echo matched
;;
esac
for j
in 1; do echo $j; done
arr=(1
2)
(echo sub
)
{ echo brace
}
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== quotes, here-docs, comments and backslash-newlines"
rm -f "$D/hist"
session <<'EOF'
cat <<END
hello

END
echo "a

b"
if true # comment
then echo c
fi
echo a \
b
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== the commands themselves ran"
grep -v '^$' "$D/screen" | grep -v 'wbsh\|exit\|Ctrl-D'

echo "=== the history file escapes newlines and backslashes"
sed 's/\t/<TAB>/' "$D/hist"

echo "=== a later session reads the entries back whole"
session <<'EOF'
history > "$HOME/listing"
EOF
head -6 "$D/listing"

echo "=== an older v2 history file still loads"
printf '#!wbsh-history-v2\n0\techo old\\none\n1\tfalse\n' > "$D/hist"
session <<'EOF'
history > "$HOME/listing"
EOF
cat "$D/listing"
head -1 "$D/hist"

echo "=== Ctrl-D in a continuation drops the half-typed entry"
rm -f "$D/hist"
printf 'for i in 1\n' | session
session <<'EOF'
history > "$HOME/listing"
EOF
cat "$D/listing"

echo "=== Up recalls a multi-line entry whole"
history -c
history -s "$(printf 'cat <<END\nx\nEND')"
__lineedit '<UP>' '<ENTER>'

rm -rf "$D"
