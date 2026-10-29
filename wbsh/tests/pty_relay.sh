#!/bin/bash
# script and unbuffer: a command on a pseudoconsole of its own sees a
# console on all three streams however the shell's are redirected, and
# its output comes back as the console renders it, CR LF line ends included.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

T=_pty_relay_tmp
rm -rf $T
mkdir -p $T

unix() {
	tr -d '\r'
}

typescript() {
	unix < "$1" | sed -e 's/ on [0-9-]* [0-9:+-]* / on DATE /' \
		-e 's/ COLUMNS="[0-9]*" LINES="[0-9]*"//' -e 's/ <not executed on terminal>//' \
		-e 's/ TERM="[^"]*"//'
}

echo "=== unbuffer"
[ -t 1 ] && echo "piped stdout is a tty" || echo "piped stdout is no tty"
unbuffer $W -r -c '[ -t 0 ] && [ -t 1 ] && [ -t 2 ] && echo "all three are ttys"' | unix
unbuffer echo hi | od -c
unbuffer $W -r -c 'exit 7'; echo "status $?"
unbuffer no-such-command-xyz; echo "status $?"
unbuffer; echo "status $?"
unbuffer seq 1 3 | unix
unbuffer seq 1 100000000 | head -2 | unix; echo "statuses ${PIPESTATUS[*]}"

echo "=== unbuffer -p"
printf 'one\ntwo\r\nthree' | unbuffer -p $W -r -c 'stty -echo; while read -r line; do echo "got $line"; done; echo "end $line"' | unix
printf 'typed\n' | unbuffer -p $W -r -c 'stty -echo; read -r line; echo "[$line]"; stty -a | sed -n 2p' | unix

echo "=== Ctrl+C"
$W -r -c '__ctrlc 300; unbuffer sleep 10; echo "status $?"'

echo "=== script"
script -qc 'echo inside; exit 3' $T/one.log | unix; echo "status $?"
typescript $T/one.log
script -eqc 'exit 3' /dev/null; echo "status $?"
script -qac 'echo again' $T/one.log > /dev/null
grep -c '^Script started' $T/one.log
script -c 'echo loud' $T/loud.log | unix
(cd $T && script -qc 'echo default' > /dev/null && typescript typescript)
printf 'echo from-input\nexit 5\n' | script -q -e -B $T/io.log > /dev/null; echo "status $?"
grep -c 'echo from-input' $T/io.log
printf 'abc\n' | script -q -I $T/in.log -c 'stty -echo; read -r x; echo "read $x"' | unix
typescript $T/in.log
script --bogus; echo "status $?"
script a b; echo "status $?"
script -qc 'echo x' $T/no/such/dir.log; echo "status $?"

rm -rf $T
