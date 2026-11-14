#!/bin/bash
# -i makes a -c or script shell interactive ($- holds i, aliases expand)
# without starting the line editor.

unset WBSH_INIT_COMMAND
WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
mkdir -p _run_tmp

case $("$WBSH" --norc -i -c 'echo $-' </dev/null) in
    *i*) echo "-c has i" ;;
    *)   echo "-c lacks i" ;;
esac

printf 'case $- in *i*) echo "script has i";; *) echo "script lacks i";; esac\n' \
    > _run_tmp/interactive_flag.sh
"$WBSH" --norc -i _run_tmp/interactive_flag.sh </dev/null

"$WBSH" --norc -i -c 'alias q="echo aliased"
q' </dev/null

case $("$WBSH" -c 'echo $-') in
    *i*) echo "plain -c has i" ;;
    *)   echo "plain -c lacks i" ;;
esac

rm -f _run_tmp/interactive_flag.sh
