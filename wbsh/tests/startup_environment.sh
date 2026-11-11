#!/bin/bash
# What a new wbsh takes from its environment: the Windows variables Git
# Bash scripts use by their upper-case names are there under those names
# (not exported a second time), and IFS from the environment is ignored,
# as in bash.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

echo "=== upper-case names"
for name in SYSTEMROOT WINDIR COMSPEC PROGRAMFILES TEMP; do
    value=$("$WBSH" -r -c "printf '%s' \"\$$name\"" < /dev/null)
    [ -n "$value" ] && echo "$name is set" || echo "$name is EMPTY"
done
echo "exported once: $("$WBSH" -r -c 'env | grep -ci "^systemroot="' < /dev/null)"

echo "=== IFS from the environment"
env IFS=x "$WBSH" -r -c 'a=1x2; set -- $a; echo "words: $#"' < /dev/null
env IFS=x "$WBSH" -r -c 'printf "%s" "$IFS" | od -An -c | tr -s " "' < /dev/null
