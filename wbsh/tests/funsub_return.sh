#!/bin/bash
# bash 5.3's ${ cmd; } runs like an unnamed function: return ends it and
# local variables stay inside.

echo ${ printf '%s\n' aa; return; echo cc; }
unset x
echo ${ printf '%s\n' aa bb; x=42; return 12; echo ee; }
echo "outside: $x"
unset x
echo ${ local x; printf '%s\n' aa; x=42; return 12; }
echo "outside: $x"
xx=${ local x; echo aa; return 12; echo bb; }
echo "assignment: $? [$xx]"
f() { echo "${ echo inner; return 3; }"; echo "f goes on: $?"; }
f
