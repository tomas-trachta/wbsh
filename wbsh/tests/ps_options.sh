#!/bin/bash
# ps selection (-p, --ppid, -C, -u, -N, -e, BSD operands), output formats
# (-o with custom headers, -f, -F, -l, aux), --sort, exit status 1 when
# nothing is selected; pgrep / pkill -f match the full command line.
# Process lists differ per machine, so only stable facts are printed.

squeeze() {
    tr -s ' ' | sed 's/^ //'
}

echo "=== -p selects, exits 1 when nothing matches"
[ "$(ps -o pid= -p $$ | squeeze)" = "$$" ] && echo "ps -p \$\$ finds the shell"
ps -p 999999 > /dev/null
echo "missing pid: $?"
ps -p 999999 | squeeze
ps $$ > /dev/null && echo "bare pid operand works"
ps -q $$ -o comm=

echo "=== -o"
ps -o comm= -p $$
ps -o pid= -o comm= -p $$ | squeeze | sed "s/^$$ /PID /"
ps -o 'pid,cmd=The Command' -p $$ | head -1 | squeeze
ps -o pid,ppid,user,comm,etime,time,rss,vsz,%cpu,%mem,stat,nlwp -p $$ | head -1 | squeeze
ps --no-headers -o comm -p $$
ps -o args= -p $$ | grep -q 'ps_options.sh' && echo "args shows the command line"
ps -o ppid= -p $$ | grep -qx ' *[0-9][0-9]*' && echo "ppid is a number"
user=$(ps -o user= -p $$)
[ -n "$user" ] && [ "$user" != "?" ] && echo "owner of the shell is known"

echo "=== standard formats"
ps -f -p $$ | head -1 | squeeze
ps -F -p $$ | head -1 | squeeze
ps -l -p $$ | head -1 | squeeze
ps u -p $$ | head -1 | squeeze
ps aux | head -1 | squeeze
ps -ef | head -1 | squeeze
ps | head -1 | squeeze

echo "=== selection"
sleep 37.25 &
child=$!
ps --ppid $$ -o pid= | grep -qx " *$child" && echo "--ppid finds the background job"
ps -C wbsh -o comm= | sort -u
ps -C wbsh.exe -o comm= | sort -u
ps -e -o pid= | grep -qx " *$$" && echo "-e lists the shell"
ps -N -p $$ -o pid= | grep -qx " *$$" || echo "-N leaves the shell out"
ps -u "$user" -o pid= | grep -qx " *$$" && echo "-u finds the shell"

echo "=== pgrep / pkill -f"
pgrep -f 'sleep 37[.]25' | grep -qx "$child" && echo "pgrep -f finds the job"
pgrep -a -f 'sleep 37[.]25' | grep -q "^$child .*sleep 37.25" && echo "pgrep -a shows its command line"
pkill -f 'sleep 37[.]25'
echo "pkill -f: $?"
wait "$child" 2> /dev/null
ps -p "$child" > /dev/null
echo "after pkill: $?"

echo "=== --sort"
ps -e -o pid= --sort=-pid | awk 'NR > 1 && $1 > prev { bad = 1 } { prev = $1 } END { print bad ? "unsorted" : "descending" }'
ps -e -o pid= --sort=pid | awk 'NR > 1 && $1 < prev { bad = 1 } { prev = $1 } END { print bad ? "unsorted" : "ascending" }'

echo "=== errors"
ps -o foo
echo "rc=$?"
ps -z
echo "rc=$?"
ps --sort=nope
echo "rc=$?"
ps -p abc
echo "rc=$?"
ps -p
echo "rc=$?"
