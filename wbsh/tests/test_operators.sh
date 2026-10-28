#!/bin/bash
# Probe: test / [ / [[ operators: -a -o ! ( ), file and option tests, -nt -ot -ef (bash 5.2 semantics).

mkdir -p _run_tmp/top
cd _run_tmp/top || exit 1
printf '#!/bin/sh\necho configured\n' > configure
printf 'plain\n' > plain.txt
: > empty.txt
mkdir -p dir

echo "=== 1. -a, -o, !, ( ) in test and ["
[ -n a -a -n b ] && echo "and ok"; echo "rc=$?"
[ -n a -a -z b ]; echo "and false rc=$?"
[ -z a -o -n b ] && echo "or ok"
[ ! -z a ] && echo "not ok"
[ \( -n a -o -z a \) -a -n b ] && echo "parens ok"
[ ! \( -n a \) ]; echo "not parens rc=$?"
test a -a ''; echo "two strings -a rc=$?"
test a -o ''; echo "two strings -o rc=$?"
[ x = x -a y != z -o 1 -eq 2 ] && echo "mixed ok"
[ \( a \) ] && echo "group of one"
[ ! ] && echo "lone bang is a string"
[ -n ] && echo "lone -n is a string"
[ = = = ] && echo "three equal signs"

echo "=== 2. errors"
[ a -a ] 2>&1; echo "rc=$?"
[ 1 -eq x ] 2>&1; echo "rc=$?"
[ a b c d e ] 2>&1; echo "rc=$?"
[ \( a ] 2>&1; echo "rc=$?"
test -q a 2>&1; echo "rc=$?"
[ a 2>&1; echo "rc=$?"

echo "=== 3. file tests"
[ -x ./configure ] && echo "#! script is executable"
[ -x plain.txt ] || echo "plain text is not"
[ -x dir ] && echo "directory is searchable"
[ -s plain.txt ] && [ ! -s empty.txt ] && echo "-s ok"
[ -e dir -a -d dir -a ! -f dir ] && echo "-e -d -f ok"
[ -L plain.txt ] || echo "plain file is not a link"
[ -h nosuch ] || echo "missing is not a link"
[ -p plain.txt ] || echo "plain file is not a pipe"
[ -S plain.txt ] || echo "plain file is not a socket"
[ -c /dev/null ] && echo "/dev/null is a char device"
[ -b /dev/null ] || echo "nor a block device"
[ -g plain.txt -o -u plain.txt -o -k plain.txt ] || echo "no setgid/setuid/sticky"
[ -O plain.txt -a -G plain.txt ] && echo "owned"
[ -t 0 ] < plain.txt || echo "file is not a terminal"
echo x | { [ -p /dev/stdin ] && echo "stdin is a pipe"; }

echo "=== 4. -nt -ot -ef in test and [["
echo old > old.txt
sleep 1
echo new > new.txt
[ new.txt -nt old.txt ] && echo "[ -nt ok"
[ old.txt -ot new.txt ] && echo "[ -ot ok"
[ new.txt -nt nosuch ] && echo "[ -nt missing ok"
[ nosuch -ot old.txt ] && echo "[ -ot missing ok"
[ plain.txt -ef ./plain.txt ] && echo "[ -ef ok"
[ plain.txt -ef new.txt ] || echo "[ -ef different"
[[ new.txt -nt old.txt && old.txt -ot new.txt ]] && echo "[[ -nt -ot ok"
[[ plain.txt -ef ./dir/../plain.txt ]] && echo "[[ -ef ok"

echo "=== 5. [[ ]] options, terminals, files"
set -e
[[ -o errexit ]] && echo "errexit on"
set +e
[[ -o errexit ]] || echo "errexit off"
[[ -o nosuchoption ]] || echo "unknown option is off"
[[ -o pipefail ]] || echo "pipefail off"
[[ -t 0 ]] < plain.txt || echo "[[ -t ]] file is not a terminal"
[[ -x ./configure && ! -x plain.txt ]] && echo "[[ -x ]] ok"
[[ -L plain.txt ]] || echo "[[ -L ]] ok"
[[ -c /dev/null ]] && echo "[[ -c ]] ok"
[[ 1+1 -eq 2 ]] && echo "[[ -eq ]] is arithmetic"
v=set; [[ -v v && ! -v nosuch_var ]] && echo "[[ -v ]] ok"
[ -v v ] && echo "[ -v ] ok"
