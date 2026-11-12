#!/bin/bash
# Arguments to .bat / .cmd files must not break out into cmd.exe syntax.
# CreateProcess runs batch files through cmd.exe, which parses the command
# line again (the "BatBadBut" class, CVE-2024-24576 and friends).
#
# show.bat only echoes %1 and %2 as they arrive (still quoted): a script
# that uses %~1 unquoted is unsafe whoever calls it, so this checks the one
# thing wbsh controls, the call line.
. "$STRESS_ROOT/lib/check.sh"

printf '@echo off\r\necho RAW1=[%%1]\r\necho RAW2=[%%2]\r\n' > show.bat

# has_line TEXT -- the output has a line that is exactly TEXT.
has_line() {
    printf '%s\n' "$RUN_OUT" | tr -d '\r' | grep -qxF -- "$1"
}

severity critical
for arg in 'a&echo INJECTED' 'a"&echo INJECTED' 'a|echo INJECTED' 'a"|echo INJECTED&"' \
           'a^&echo INJECTED' '"&echo INJECTED&"' 'a\"&echo INJECTED' '"' 'a\'; do
    wbsh_run "./show.bat $(printf '%q' "$arg") second"
    if has_line 'INJECTED'; then
        fail "batch argument '$arg' runs a second command"
    elif has_line 'RAW2=[second]'; then
        ok "batch argument '$arg' stays one argument"
    else
        fail "batch argument '$arg' shifts the other arguments: $(printf '%s' "$RUN_OUT" | tr '\n' ' ')"
    fi
done
rm -f pwned.txt
wbsh_run "./show.bat 'x>pwned.txt' second"
check "batch argument with '>' does not create a file" [ ! -e pwned.txt ]
check "batch argument with '>' arrives quoted" has_line 'RAW1=["x>pwned.txt"]'

severity high
wbsh_run './show.bat "%OS%" second'
check "%VAR% in a batch argument is not expanded" has_line 'RAW1=["%OS%"]'
wbsh_run './show.bat "$(printf "a\nb")" second; echo "rc=$?"'
expect_no_match "an argument with a line break does not run the batch file" "$RUN_OUT" "*RAW*"
expect_match "an argument with a line break is an error (126)" "$RUN_OUT" "*rc=126*"

severity medium
wbsh_run './show.bat "a b" c'
check "quoted argument with a space reaches the batch file" has_line 'RAW1=["a b"]'
wbsh_run './show.bat install'
check "a plain word reaches the batch file without quotes" has_line 'RAW1=[install]'
wbsh_run 'PATH="$PWD:$PATH"; show.bat x'
check "a batch file found on PATH runs" has_line 'RAW1=[x]'
finish
