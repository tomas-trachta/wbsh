#!/bin/bash
# nohup, setsid, nice, renice, su, sudo and top. Commands that need a
# terminal run through `setsid -w ... </dev/null`, which puts them on a
# hidden console of their own; there CONIN$ / CONOUT$ are a terminal for
# nohup and top. Nothing opens a window and nothing asks for elevation:
# sudo only ever finds the fake one this script puts on PATH.

D=_session_tmp
rm -rf $D
mkdir -p $D/empty $D/fake
cd $D

wait_for() {
	local i
	for i in $(seq 1 100); do
		[ -s "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

echo "=== nice prints and sets the niceness (priority class)"
nice
nice nice
nice -n 5 nice
nice -19 nice
nice -n 19 nice
nice --adjustment=12 nice
nice -n 100 nice
nice -n -5 nice
nice --5 nice
nice -n -20 nice
in_function() { echo "function at $(nice)"; return 3; }
nice -n 4 in_function; echo "status $?"

echo "=== nice errors"
nice -n 5; echo "status $?"
nice -n x true; echo "status $?"
nice -q true; echo "status $?"
nice no_such_command_xyz; echo "status $?"

echo "=== renice, read back by ps and top"
sleep 30 &
victim=$!
mask() { sed "s/^$victim /VICTIM /"; }
renice 10 -p $victim | mask
renice -n 19 $victim | mask
renice --relative -10 $victim | mask
ps -o ni= -p $victim
ps -o pri= -o opri= -o priority= -o intpri= -p $victim
ps -l -p $victim | awk 'NR == 2 { print "ps -l PRI " $7 " NI " $8 }'
top -b -n 1 -d 0 -p $victim | awk 'NR == 8 { print "top PR " $3 " NI " $4 }'
renice 0 -g $victim | mask
ps -o ni= -p $victim

echo "=== renice errors"
renice 5 999999; echo "status $?"
renice 5 abc; echo "status $?"
renice 5; echo "status $?"
renice x 1; echo "status $?"
renice 5 -u no_such_user_xyz; echo "status $?"
renice 5 -g 999999; echo "status $?"

echo "=== renice 0 is the shell itself"
renice 12 0
nice
renice 0 -p 0
renice --relative 4 -g 0 | cat
renice 0 0 > /dev/null
nice
kill $victim
wait $victim 2> /dev/null

echo "=== setsid"
say() { echo "out: $*"; echo "err: $*" >&2; return 7; }
setsid -w say hello; echo "status $?"
setsid say a script waits; echo "status $?"
setsid; echo "status $?"
setsid -x true; echo "status $?"
setsid no_such_command_xyz; echo "status $?"
late() { sleep 0.5; echo late > late.txt; }
setsid -f late; echo "status $?"
[ -f late.txt ] && echo "waited" || echo "returned at once"
wait_for late.txt && cat late.txt

echo "=== Ctrl+C gives the shell back and leaves the command running"
"$BASH" -r -c 'late() { sleep 1; echo "still ran" > kept.txt; }
__ctrlc 300; setsid -w late; echo "setsid -w: $?"'
wait_for kept.txt && cat kept.txt
"$BASH" -r -c 'late() { sleep 1; echo "still ran" > kept2.txt; }
__ctrlc 300; nohup late < /dev/null > /dev/null; echo "nohup: $?"'
wait_for kept2.txt && cat kept2.txt

echo "=== nohup without a terminal changes nothing"
nohup say plain < /dev/null; echo "status $?"
nohup -- echo after dashdash < /dev/null
ls nohup.out 2> /dev/null || echo "no nohup.out"

echo "=== --help and --version"
nohup --help | head -1
nohup --version | sed 's/[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/X.Y.Z/'
setsid --help | head -1
setsid -h | sed -n 2p
setsid --version | sed 's/[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$/X.Y.Z/'
setsid -V > /dev/null; echo "status $?"

echo "=== nohup errors"
nohup; echo "status $?"
POSIXLY_CORRECT=1 nohup; echo "status $?"
nohup -x true; echo "status $?"
nohup no_such_command_xyz; echo "status $?"

echo "=== nohup on a terminal"
all_tty() { nohup say on a terminal <CONIN$ 1<>CONOUT$ 2> err.txt; echo "status $?" >> err.txt; }
setsid -w all_tty < /dev/null
cat err.txt nohup.out
setsid -w all_tty < /dev/null
echo "appended: $(grep -c terminal nohup.out)"
rm nohup.out
out_tty() { nohup echo input not a terminal 1<>CONOUT$ 2> err.txt; }
setsid -w out_tty < /dev/null
cat err.txt nohup.out
rm nohup.out
in_tty() { nohup say stdin only <CONIN$ > out.txt 2> err.txt; }
setsid -w in_tty < /dev/null
cat err.txt out.txt
err_tty() { nohup say stderr follows stdout <CONIN$ > out.txt 2<>CONOUT$; }
setsid -w err_tty < /dev/null
cat out.txt
ls nohup.out 2> /dev/null || echo "no nohup.out"

echo "=== nohup falls back to \$HOME/nohup.out"
mkdir -p home nohup.out home2/nohup.out
HOME=$PWD/home setsid -w all_tty < /dev/null
sed "s#$PWD#PWD#" err.txt
cat home/nohup.out
HOME=$PWD/home2 setsid -w all_tty < /dev/null
sed "s#$PWD#PWD#" err.txt
rmdir nohup.out

echo "=== su and sudo"
su; echo "status $?"
su - alice -c true; echo "status $?"
PATH=$PWD/empty sudo net session; echo "status $?"
printf '@echo fake sudo: %%*\r\n@exit /b 3\r\n' > fake/sudo.cmd
PATH=$PWD/fake sudo net session /list | tr -d '\r'; echo "status ${PIPESTATUS[0]}"
PATH=$PWD/fake sudo -E whoami | tr -d '\r'
PATH=$PWD/fake sudo cd / | tr -d '\r' | sed 's/fake sudo: .*\.exe -r -c/fake sudo: WBSH -r -c/'

echo "=== top -b layout"
top -b -n 1 -d 0.2 > top.txt
sed -n 1p top.txt | grep -q '^top - [0-9][0-9]:[0-9][0-9]:[0-9][0-9] up .* load average: ' \
	&& echo "uptime line"
sed -n 2p top.txt | grep -q '^Tasks: *[0-9]* total, *[0-9]* running, *[0-9]* sleeping, ' \
	&& echo "tasks line"
sed -n 3p top.txt | grep -q '^%Cpu(s): .* us, .* sy, .* id, ' && echo "cpu line"
sed -n 4p top.txt | grep -q '^MiB Mem : .* total, .* free, .* used, .* buff/cache$' \
	&& echo "memory line"
sed -n 5p top.txt | grep -q '^MiB Swap: .* total, .* free, .* used\. .* avail Mem$' \
	&& echo "swap line"
sed -n 6,7p top.txt
awk 'NR == 4 { d = $4 - ($6 + $8 + $10); print (d < 1 && d > -1) ? "memory adds up" : "memory off" }' top.txt
awk 'NR == 3 { s = $2 + $4 + $8; print (s > 99 && s < 101) ? "cpu adds up" : "cpu off: " s }' top.txt
awk 'NR == 2 { t = $2 } NR > 7 { n++ } END { print (n == t) ? "every task listed" : "rows " n " of " t }' top.txt
awk 'NR > 7 && NF < 11 { bad = 1 } END { print bad ? "short row" : "rows complete" }' top.txt

echo "=== top selection, sorting and iterations"
top -b -n 1 -d 0 -p $$ | awk 'NR > 7 { print ($1 == pid) ? "the shell" : "other " $1 }' pid=$$
top -b -n 1 -d 0 -p $$ -c | tail -1 | grep -q 'session_cmds\.sh' && echo "-c shows the command line"
top -b -n 1 -d 0 -p 999999 | wc -l
top -b -n 1 -d 0 -o -PID | awk 'NR > 8 && $1 < prev { bad = 1 } NR > 7 { prev = $1 }
	END { print bad ? "unsorted" : "PID ascending" }'
top -b -n 1 -d 0 -o PID | awk 'NR > 8 && $1 > prev { bad = 1 } NR > 7 { prev = $1 }
	END { print bad ? "unsorted" : "PID descending" }'
top -b -n 1 -d 0 -o %MEM | awk 'NR > 8 && $9 > prev { bad = 1 } NR > 7 { prev = $9 }
	END { print bad ? "unsorted" : "%MEM descending" }'
me=$(ps -o user= -p $$)
top -b -n 1 -d 0 -u "$me" | awk 'NR > 7 && $2 != user { bad = 1 }
	END { print bad ? "other users" : "only the user" }' user="$me"
top -b -n 3 -d 0.1 | grep -c '^top - '
top -n 1 -d 0 | head -1 | cut -c1-5

echo "=== top errors"
top -b -n 0; echo "status $?"
top -b -d x; echo "status $?"
top -b -d -1; echo "status $?"
top -b -o BOGUS; echo "status $?"
top -b -p 1,x; echo "status $?"
top -b -z; echo "status $?"
top -b extra; echo "status $?"

echo "=== top on a terminal"
interactive() { top -n 2 -d 0.1 <CONIN$ 1<>CONOUT$; echo "status $?" > top.txt; }
setsid -w interactive < /dev/null
cat top.txt

cd ..
rm -rf $D
echo "=== done"
