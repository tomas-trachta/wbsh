#!/bin/bash
# An interactive shell keeps PS1 and PS2 inherited from the environment, even
# empty ones, and fills in its defaults only when they are unset. With
# --noediting a piped line is read as is, so only the prompts reach stderr;
# with the line editor the line is echoed after its prompt. The expected
# texts are GNU bash 5.3's.

unset WBSH_INIT_COMMAND
WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

stderr_of() {
    printf 'echo hi\n' | "$WBSH" --norc "$@" -i 2>&1 >/dev/null
}

check() {
    [ "$2" = "$3" ] && echo "$1: ok" || echo "$1: got [$2]"
}

check "empty PS1, --noediting" "$(PS1= PS2= stderr_of --noediting)" "exit"
check "custom PS1, --noediting" "$(PS1='X> ' PS2= stderr_of --noediting)" "X> X> exit"
check "custom PS1, editing" "$(PS1='X> ' PS2= stderr_of)" $'X> echo hi\nX> exit'

defaulted=$(unset PS1 PS2; stderr_of --noediting)
[ "$defaulted" != "exit" ] && echo "unset PS1: default prompt" || echo "unset PS1: no prompt"
