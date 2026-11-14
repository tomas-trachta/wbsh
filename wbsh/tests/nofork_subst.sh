#!/bin/bash
# bash 5.3's ${ cmd; } runs cmd in the current shell and substitutes its
# output; ${| cmd; } substitutes what cmd left in REPLY, which stays local.

echo "=== side effects stay in the shell"
x=${ echo captured; y=assigned; }
echo "x=[$x] y=$y"

counter=0
bump() { counter=$((counter + 1)); echo "$counter"; }
first=${ bump; }
second=${ bump; }
echo "first=$first second=$second counter=$counter"

echo "=== forms"
nl=${
echo newline-form
}
echo "$nl"
tab=${	echo tab-form; }
echo "$tab"
echo "quoted: ${ printf '%s' 'a}b'; }"
multi=${ echo one; echo two; }
echo "$multi"
trailing=${ printf 'x\n\n\n'; }
echo "[$trailing]"

echo "=== exit status"
s=${ false; }
echo "status $?"
s=${ true; }
echo "status $?"

echo "=== REPLY form"
REPLY=outer
r=${| REPLY=inner; }
echo "r=$r REPLY=$REPLY"
unset REPLY
r=${| REPLY=$((6 * 7)); }
echo "r=$r REPLY=${REPLY-unset}"
r=${| echo "printed, not captured"; }
echo "r=[$r]"

echo "=== inside functions"
f() {
	local v=${ echo in-function; local_in=yes; }
	echo "$v $local_in"
}
f
