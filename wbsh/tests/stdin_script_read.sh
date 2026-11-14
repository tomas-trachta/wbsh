#!/bin/bash
# A script read from standard input is read a command at a time, so a
# command in it that reads standard input gets the script's next lines:
# byte by byte from a pipe, with the descriptor put back from a file.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
mkdir -p _run_tmp

printf 'read x\nhello\necho "[$x]"\n' | "$WBSH"
printf 'read x\nhello\necho "[$x]"\n' > _run_tmp/stdin_read.sh
"$WBSH" < _run_tmp/stdin_read.sh
"$WBSH" -s < _run_tmp/stdin_read.sh

printf 'read a; read b\nfirst\nsecond\necho "[$a|$b]"\n' > _run_tmp/stdin_two.sh
"$WBSH" < _run_tmp/stdin_two.sh
printf 'while read -r l; do echo "got $l"; [ "$l" = stop ] && break; done\nx\nstop\necho done\n' \
    | "$WBSH"

printf 'f() {\n  echo "in f: $1"\n}\nf one\nif true; then\n  echo multi\nfi\necho "line $LINENO"\n' \
    | "$WBSH"
printf 'echo ok\nfi\necho "never"\n' | "$WBSH" 2>&1
echo "syntax status: $?"
printf 'cat <<E\nheredoc body\nE\necho "a \\n b"\n' | "$WBSH"
printf 'exit 7\necho never\n' | "$WBSH"; echo "exit status: $?"

rm -f _run_tmp/stdin_read.sh _run_tmp/stdin_two.sh
