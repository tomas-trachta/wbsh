#!/bin/bash
# Arguments to .bat / .cmd files must not break out into cmd.exe syntax.
# CreateProcess runs batch files through cmd.exe, which re-parses the
# command line (the "BatBadBut" class, CVE-2024-24576 and friends).
. "$STRESS_ROOT/lib/check.sh"

printf '@echo off\r\necho ARG1=[%%~1]\r\necho ARG2=[%%~2]\r\n' > show.bat

severity critical
for arg in 'a&echo INJECTED' 'a"&echo INJECTED' 'a|echo INJECTED' 'a"|echo INJECTED&"' \
           'a^&echo INJECTED' '"&echo INJECTED&"' 'a\"&echo INJECTED'; do
    wbsh_run "./show.bat $(printf '%q' "$arg") second"
    if printf '%s\n' "$RUN_OUT" | grep -qx 'INJECTED'; then
        fail "batch argument '$arg' runs a second command"
    else
        ok "batch argument '$arg' stays one argument"
    fi
done
rm -f pwned.txt
wbsh_run "./show.bat 'x>pwned.txt' second"
check "batch argument with '>' does not create a file" [ ! -e pwned.txt ]

severity medium
wbsh_run './show.bat "%OS%" second'
case "$RUN_OUT" in
    *Windows_NT*) review "batch arguments with %VAR% are expanded by cmd.exe (%OS% became Windows_NT); document or escape" ;;
    *) ok "%VAR% in batch arguments is not expanded" ;;
esac
wbsh_run './show.bat "a b" "c"'
expect_match "quoted argument with a space reaches the batch file" "$RUN_OUT" "*ARG1=[a b]*"
finish
