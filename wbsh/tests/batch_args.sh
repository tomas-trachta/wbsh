#!/bin/bash
# Arguments reach a .bat / .cmd file as one quoted argument each, and cmd.exe
# runs nothing from them: no & | < > breakout, no %VAR% expansion, no `"`
# that ends the quotes (CVE-2024-24576). A line break cannot pass at all.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
printf '@echo off\r\necho 1=[%%1]\r\necho 2=[%%2]\r\n' > show.bat
cp show.bat show.cmd

show() {
    ./show.bat "$@" 2>&1 | tr -d '\r'
}

echo "=== metacharacters stay inside the argument"
show 'a&echo INJECTED' second
show 'a|echo INJECTED' second
show 'x>made.txt' second
[ -e made.txt ] && echo "made.txt exists" || echo "no file made"

echo "=== a quote cannot end the quoting"
show '"&echo INJECTED&"' second
show 'a\"&echo INJECTED' second
show '"' second

echo "=== percent signs are not expanded"
show '%OS%' '100%'

echo "=== backslashes, empty and spaced arguments"
show 'a\' 'C:\dir\'
show '' 'a b'

echo "=== ./x.cmd and a PATH lookup both run"
./show.cmd one | tr -d '\r'
( PATH="$PWD:$PATH"; show.bat two | tr -d '\r' )

echo "=== a line break is refused"
./show.bat "$(printf 'a\nb')" 2>&1 | tr -d '\r'
./show.bat "$(printf 'a\nb')" > /dev/null 2>&1
echo "status $?"

cd / && rm -rf "$D"
