#!/bin/bash
# Probe: redirections put closed fds back closed, and >&N to a closed fd fails (bash 5.2 semantics).

mkdir -p _run_tmp
cd _run_tmp || exit 1
rm -f lk.txt lk2.txt

fd_error() {
	local message="$1"
	local head="${message%: *}"
	echo "${head##*: }: ${message##*: }"
}

try() {
	local err
	err=$(eval "$1" 2>&1 >/dev/null)
	local rc=$?
	echo "$1 -> rc=$rc${err:+ error=$(fd_error "$err")}"
}

echo "=== 1. a redirection of a closed fd does not leave the file open"
true 3>lk.txt; mv lk.txt lk2.txt && echo "moved after 3>"
echo a 5>lk2.txt 6>&5; rm lk2.txt && echo "removed after 5> 6>&5"
{ :; } 4>lk.txt; mv lk.txt lk2.txt && echo "moved after { } 4>"
(:) 7>>lk2.txt; rm lk2.txt && echo "removed after ( ) 7>>"
f() { :; }; f 8<>lk.txt; mv lk.txt lk2.txt && echo "moved after f 8<>"; rm -f lk2.txt
cat 3<<<here </dev/null >/dev/null; echo "here-string on 3 rc=$?"
exec 3>lk.txt; echo to3 >&3; exec 3>&-; cat lk.txt; mv lk.txt lk2.txt && echo "moved after exec 3>&-"
rm -f lk2.txt

echo "=== 2. >&N and <&N from a closed fd fail the command"
try 'echo x >&7'
try 'echo y 2>&7'
try 'cat <&8'
try '{ echo inner >&3; } >/dev/null'
try 'x=$(echo sub >&3)'
try 'exec 4>&1; exec 4>&-; echo closed4 >&4'
echo x 2>/dev/null >&7 || echo "|| runs after the failure"

echo "=== 3. valid duplications still work"
try 'echo z 3>&3'
f() { echo "f out" >&5; }; f 5>&1
( echo "sub to 9" >&9 ) 9>&1
exec 4>&1; echo "via 4" >&4; exec 4>&-
{ echo "err to out" >&2; } 2>&1
{ echo "moved stderr" >&2 2>&- 2>&1; } 2>&1

echo "=== 4. exec redirections end with the subshell or script that made them"
( exec 3>f3.txt; echo in3 >&3 ); try 'echo leaked >&3'
x=$(exec 4>f4.txt; echo in4 >&4); try 'echo leaked >&4'
rm f3.txt f4.txt && echo "removed f3 f4"
( exec >/dev/null; echo hidden ); echo "visible after ( exec >/dev/null )"
x=$(exec >/dev/null; echo hidden); echo "x=[$x]"
printf 'exec >log.txt\necho to-log\n' > s.sh; chmod +x s.sh; ./s.sh; echo "stdout back after ./s.sh"; cat log.txt
rm s.sh log.txt && echo "removed s.sh log.txt"
f() { exec 5>f5.txt; }; f; echo "in5" >&5; exec 5>&-; cat f5.txt; rm f5.txt && echo "removed f5"
