#!/bin/bash
# The logout builtin: wbsh is never a login shell. In a script it is bash's
# error; typed at the prompt of an interactive shell it ends it like exit.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

echo "=== in a script"
logout; echo "status $?"
logout 3; echo "status $?"
type -t logout

echo "=== interactive"
D=$(mktemp -d) || exit 1
session() {
    env -u WBSH_INIT_COMMAND HOME="$D" HISTFILE="$D/hist" PS1='' "$WBSH" -i > "$D/screen" 2>&1
}

printf 'echo before\nlogout\necho after\n' | session
echo "exit status $?"
grep -c after "$D/screen"

printf 'logout 7\n' | session
echo "exit status $?"
rm -rf "$D"
