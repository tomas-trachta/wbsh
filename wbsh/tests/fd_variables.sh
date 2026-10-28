#!/bin/bash
# {var}>file, {var}<file, {var}>>file, {var}<>file, {var}>&N and {var}>&-:
# the shell picks a descriptor from 10 up and stores it in var. The probe
# runs under a timeout because it waits for a coprocess.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
P=_run_tmp/fd_variables_probe.sh
cat > $P <<'EOF'
F=_run_tmp/fd_variables.txt
rm -f $F
exec {fd}>/dev/null; echo "fd=$fd"; exec {fd}>&-
exec {out}>$F
echo "first" >&$out
echo "second" >&$out
exec {out}>&-
cat $F
exec {in}<$F
read -r a <&$in; read -r b <&$in
echo "read [$a] [$b]"
exec {in}<&-
exec {app}>>$F; echo third >&$app; exec {app}>&-
tail -n 1 $F
exec {rw}<>$F; echo "rw=$rw"; exec {rw}>&-
exec {a1}>/dev/null {a2}>/dev/null; echo "two: $((a2 > a1))"; exec {a1}>&- {a2}>&-
exec {dup}>&1; echo "via dup" >&$dup; exec {dup}>&-
echo hi {tmp}>$F; echo "persisted: $([ -n "$tmp" ] && echo yes)"; exec {tmp}>&-
cat $F
declare -a fds; exec {fds[2]}>/dev/null; echo "elem=${fds[2]}"; exec {fds[2]}>&-
coproc cat
exec {COPROC[1]}>&-
wait $COPROC_PID; echo "coproc saw EOF"
rm -f $F
EOF

timeout 60 $W -r $P > _run_tmp/fd_variables_probe.log 2>&1
echo "rc=$?"
cat _run_tmp/fd_variables_probe.log
