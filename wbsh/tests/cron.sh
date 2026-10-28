#!/bin/bash
# crontab and crond: schedule fields and next runs (daylight saving in
# named Windows zones), installing, listing, checking, editing and
# removing crontabs in a -c directory, jobs run by crond --once, the
# one-crond-per-directory lock, and stopping crond.

export USER=tester
tmp=$(mktemp -d)
cd "$tmp" || exit 1
mkdir home
export TMPDIR=$tmp

crond_pid=
trap '[ -n "$crond_pid" ] && kill $crond_pid 2>/dev/null' EXIT

next() {
	crontab --next-runs "$@"
	echo "rc=$?"
}

stripped() {
	sed 's/^[A-Z][a-z][a-z] [0-9 ][0-9] [0-9:]* crond\[[0-9]*\]: //; s/pid [0-9]*/pid N/g' "$@"
}

echo "=== fields: lists, ranges, steps, names"
next '*/15 * * * *' '2026-01-15 12:07' 3 UTC
next '5,10-12 */6 * * *' '2026-01-15 12:07' 6 UTC
next '10/20 3 * * *' '2026-01-15 00:00' 4 UTC
next '0 0 1 JAN,jul *' '2026-01-15 00:00' 3 UTC
next '0 0 * * 7' '2026-01-15 00:00' 2 UTC
next '0 0 * * fri-sun' '2026-01-15 00:00' 4 UTC
next '0 9 * * Mon-Fri' '2026-01-15 00:00' 3 UTC
next '0 0 * * */3' '2026-01-15 00:00' 3 UTC
next '59 23 31 12 *' '2026-01-15 00:00' 2 UTC

echo "=== day of month or day of week"
next '0 12 13 * fri' '2026-01-01 00:00' 4 UTC
next '0 12 */10 * fri' '2026-01-01 00:00' 3 UTC
next '0 12 13 * *' '2026-01-01 00:00' 2 UTC

echo "=== @keywords"
for spec in @yearly @annually @monthly @weekly @daily @midnight @hourly @reboot; do
	next "$spec" '2026-01-15 12:07' 2 UTC
done

echo "=== dates that come rarely or never"
next '0 0 29 2 *' '2026-01-01 00:00' 3 UTC
next '0 0 30 2 *' '2026-01-01 00:00' 3 UTC
next '0 0 31 4,6 *' '2026-01-01 00:00' 1 UTC

echo "=== daylight saving: a skipped hour"
cet='Central Europe Standard Time'
next '30 2 * * *' '2026-03-28 12:00' 3 "$cet"
next '*/20 2 * * *' '2026-03-29 00:00' 3 "$cet"
next '0 * * * *' '2026-03-29 00:30' 3 "$cet"
next '30 2 * * *' '2026-03-07 12:00' 3 'Pacific Standard Time'

echo "=== daylight saving: a repeated hour"
next '30 2 * * *' '2026-10-24 12:00' 3 "$cet"
next '*/20 2 * * *' '2026-10-25 00:00' 7 "$cet"
next '0 * * * *' '2026-10-25 00:30' 4 "$cet"
next '0 1 * * *' '2026-10-31 12:00' 3 'Pacific Standard Time'

echo "=== New Year takes the new year's rules (Volgograd went from +04 to +03 for 2021)"
next '0 0 1 1 *' '2020-12-31 12:00' 1 'Volgograd Standard Time'
next '0 12 1 1 *' '2020-12-31 12:00' 1 'Volgograd Standard Time'

echo "=== bad schedules"
for spec in '60 * * * *' '* 24 * * *' '* * 0 * *' '* * 32 * *' '* * * 13 *' \
		'* * * * 8' '5-1 * * * *' '*/0 * * * *' '1,,2 * * * *' 'x * * * *' \
		'* * * foo *' '* * *' '@sometimes' '* * * * * extra'; do
	next "$spec" '2026-01-01 00:00'
done
next '* * * * *' '2026-02-30 00:00'
next '* * * * *' '2026-01-01 00:00' 0
next '* * * * *' '2026-01-01 00:00' 1 'No Such Zone'
next '* * * * *'

echo "=== install, list, remove"
crontab -c tabs -l; echo "rc=$?"
crontab -c tabs -r; echo "rc=$?"
printf '%s\n' '# a comment' '' 'MAILTO=""' '*/5 * * * * echo hi' > good
crontab -c tabs good; echo "rc=$?"
crontab -c tabs -l; echo "rc=$?"
crontab -c tabs -u TESTER -l > /dev/null; echo "same user rc=$?"
crontab -c tabs -u someone -l; echo "rc=$?"
ls -A tabs
printf '@daily date' | crontab -c tabs -; echo "rc=$?"
crontab -c tabs -l | od -c | sed 's/  */ /g'
echo n | crontab -c tabs -ri; rc=$?; echo
echo "rc=$rc"
printf 'maybe\nY\n' | crontab -c tabs -ri; rc=$?; echo
echo "rc=$rc"
crontab -c tabs -l; echo "rc=$?"
crontab -c tabs missing-file; echo "rc=$?"

echo "=== syntax errors"
printf '%s\n' '0 0 * * *' 'x * * * * a' '1 2 3 4 8 b' '@foo x' '1 2 3 13 * c' \
	'0 24 * * * d' '0 0 0 * * e' 'NAME=fine' '0 0 * * * fine' > bad
crontab -c tabs bad; echo "rc=$?"
crontab -c tabs -l; echo "rc=$?"
crontab -T bad; echo "rc=$?"
crontab -T good; echo "rc=$?"
printf '0 12 * * * echo crlf\r\nX = 1\r\n' > crlf
printf '\357\273\277@hourly echo bom\n' > bom
crontab -T crlf; echo "rc=$?"
crontab -T bom; echo "rc=$?"

echo "=== usage errors"
crontab -c tabs 2>&1 | head -2
crontab -c tabs -l -r 2>&1 | head -1
crontab -c tabs -l extra 2>&1 | head -1
crontab -c tabs good good 2>&1 | head -1
crontab -x 2>&1 | head -1
crontab --help | head -3

echo "=== editing"
append_job() { echo '0 * * * * appended' >> "$1"; }
append_bad() { echo 'oops' >> "$1"; }
fix_bad() { sed -i 's/^oops$/0 0 * * * fixed/' "$1"; }
EDITOR=append_job crontab -c tabs -e; echo "rc=$?"
crontab -c tabs -l
EDITOR=true crontab -c tabs -e; echo "rc=$?"
EDITOR=false crontab -c tabs -e; echo "rc=$?"
VISUAL=false EDITOR=true crontab -c tabs -e; echo "rc=$?"
temp_name='s|"[^"]*crontab\.[^"]*"|"TEMP"|; s|left in .*|left in TEMP|'
echo n | EDITOR=append_bad crontab -c tabs -e > edit.out 2>&1
echo "rc=$?"
sed "$temp_name" edit.out
ls crontab.* | wc -l
rm -f crontab.*
editor_run=0
second_try() {
	editor_run=$((editor_run + 1))
	if [ $editor_run -eq 1 ]; then append_bad "$1"; else fix_bad "$1"; fi
}
printf 'what\ny\n' | EDITOR=second_try crontab -c tabs -e > edit.out 2>&1
echo "rc=$?"
sed "$temp_name" edit.out
crontab -c tabs -l

echo "=== the crontab directory"
WBSH_CRONTAB_DIR=$tmp/envtabs crontab -l; echo "rc=$?"
echo '@daily true' | WBSH_CRONTAB_DIR=$tmp/envtabs crontab -
ls envtabs

echo "=== crond --once"
cat > jobs <<EOF
@reboot echo rebooted
0 12 * * * echo "in \$(pwd) as \$LOGNAME/\$USER"
1 12 * * * cat%line one%line two\\%x
FOO = "bar baz"
2 12 * * * echo "FOO=\$FOO"; exit 3
HOME=$tmp/home
3 12 * * * pwd; echo "to stderr" >&2
MAILTO=""
4 12 * * * echo discarded
MAILTO=$tmp/mailbox
5 12 * * * echo to the mailbox
SHELL=/bin/sh
6 12 * * * echo "shell \$SHELL"
EOF
crontab -c run jobs; echo "rc=$?"
for minute in 00 01 02 03 04 05 06 07; do
	crond -c run -l 0 --once "2026-01-15 12:$minute"; echo "12:$minute rc=$?"
done
stripped run/.crond.log | sed "s|$tmp|TMP|; s|$HOME|HOME|"
echo "--- mailbox"
stripped mailbox
ls -A run

echo "=== crond --once with CRLF lines, -L and -d"
crontab -c crlfrun crlf
crond -c crlfrun --once '2026-01-15 12:00' -L crlf.log; echo "rc=$?"
grep -A1 'output of' crlf.log | tail -1 | od -c | sed 's/  */ /g'
crond -c crlfrun --once '2026-01-15 12:00' -d 8 2>&1 | stripped

echo "=== crond options"
crond -x 2>&1 | head -1
crond -l high 2>&1 | head -1
crond -c run --once 'tomorrow' 2>&1 | head -1
crond extra 2>&1 | head -1
crond --help | head -1

echo "=== one crond per directory, and stopping it"
crond -c lock; echo "rc=$?"
i=0
while [ ! -s lock/.crond.pid ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
crond_pid=$(cat lock/.crond.pid)
[ -n "$crond_pid" ] && echo "pid file written"
running='s|for .* (pid [0-9]*)|for DIR (pid N)|'
crond -c lock > second.out 2>&1; echo "rc=$?"
sed "$running" second.out
crond -f -c lock --once '2026-01-15 12:00' > second.out 2>&1; echo "rc=$?"
sed "$running" second.out
ps -p $crond_pid | tail -n +2 | awk '{ print $NF }'
kill $crond_pid; echo "kill rc=$?"
i=0
while kill -0 $crond_pid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
kill -0 $crond_pid 2>/dev/null || echo "crond gone"
crond_pid=
crond -c lock --once '2026-01-15 12:00'; echo "stale pid file rc=$?"
[ -e lock/.crond.pid ] || echo "pid file removed"

echo "=== simultaneous starts leave one crond"
for i in 1 2 3 4; do crond -c race 2> race.$i.err & done
wait
cat race.*.err | grep -c "already running"
crond_pid=$(cat race/.crond.pid)
kill -0 $crond_pid && echo "the recorded crond runs"
kill $crond_pid
i=0
while kill -0 $crond_pid 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
crond_pid=

echo "=== Ctrl+C stops crond -f"
timeout 20 "$BASH" -r -c '__ctrlc 1500; crond -f -c ctrlc; echo "crond rc=$?"'
echo "timeout rc=$?"
[ -e ctrlc/.crond.pid ] || echo "pid file removed"
stripped ctrlc/.crond.log

cd / && rm -rf "$tmp"
