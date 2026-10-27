#!/bin/bash
# Probe: ${x?msg} and `set -u` errors end a non-interactive shell (bash 5.2 semantics).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

run() {
	$W -r -c "$1" 2>&1
	echo "[rc=$?]"
}

mkdir -p _run_tmp
PROBE=_run_tmp/expansion_errors_probe

echo "=== 1. \${x?msg} exits with status 1 and names the parameter"
run ': ${zz?custom msg}; echo reached'
run ': ${zz:?}; echo reached'
run ': ${zz?}; echo reached'
run 'x=; : ${x:?is empty}; echo reached'
run 'x=; : ${x?}; echo "reached: set but empty"'
run 'set --; : ${@?empty list}; echo reached'
run 'a=(); : ${a[@]?no elements}; echo reached'
run ': "${zz?quoted}"; echo reached'
run 'echo ${zz:?boom} || echo or; echo reached'

echo "=== 2. set -u exits with status 1"
run 'set -u; echo $nope; echo reached'
run 'set -u; echo ${nope}; echo reached'
run 'set -u; echo "$1"; echo reached'
run 'set -u; echo ${10}; echo reached'
run 'set -u; a=(1); echo "${a[5]}"; echo reached'
run 'set -u; declare -A h; echo "${h[k]}"; echo reached'
run 'set -u; echo ${#nope}; echo reached'
run 'set -u; echo ${#a[5]}; echo reached'
run 'set -u; echo $(( nope + 1 )); echo reached'
run 'set -u; (( nope++ )); echo reached'
run 'set -u; let "nope + 1"; echo reached'
run 'set -u; a=(1); echo ${a[nope]}; echo reached'
run 'set -u; x=nope; echo ${!x}; echo reached'

echo "=== 3. what set -u leaves alone"
run 'set -u; echo "$@" "$*" "${nope[@]}" ok'
run 'set -u; echo ${nope:-d} ${nope-d} [${nope+a}] ${2-d} ${2:-d} [${2+a}]'
run 'set -u; set -- a; echo $1 ${1} $#'
run 'set -u; (( nope = 3 )); echo "assigned $nope"'
run 'set -u; a=(1); echo "$(( a[5] + 1 ))"'
run 'set -u; echo $(( RANDOM >= 0 )) $(( SECONDS >= 0 ))'
run 'set -u; cat <<< "$nope"; echo "here-string fails only its command"'

echo "=== 4. every context exits"
run 'set -u; x=$nope; echo reached'
run 'set -u; nope2=$nope echo hi; echo reached'
run 'set -u; echo ok > "$nope"; echo reached'
run 'set -u; case $nope in *) ;; esac; echo reached'
run 'set -u; case x in $nope) ;; esac; echo reached'
run 'set -u; [[ $nope ]]; echo reached'
run 'set -u; for x in $nope; do :; done; echo reached'
run 'set -u; for i in 1; do echo $nope; done; echo reached'
run 'set -u; if [ -n "$nope" ]; then :; fi; echo reached'
run 'set -u; echo $nope || echo or; echo reached'
run 'set -u; export E=$nope; echo reached'
run 'set -u; f() { local L=$nope; echo in-f; }; f; echo reached'
run 'set -u; a[$nope]=1; echo reached'
run 'eval ": \${zz?from eval}"; echo reached'
run 'trap "echo trapped \$?" EXIT; : ${zz?m}; echo reached'

echo "=== 5. a subshell, \$(...) or pipeline element ends just itself"
run '( : ${zz?inner} ); echo "after subshell $?"'
run 'set -u; (echo $nope; echo in); echo "after subshell $?"'
run 'set -u; echo "[$(echo $nope; echo in)]" done'
run 'set -u; echo $nope | cat; echo "after pipeline"'
run 'set -u; cat < /dev/null | echo $nope; echo "after pipeline"'

echo "=== 6. scripts and sourced files"
printf ': ${zz?custom msg}\necho reached\n' > $PROBE
$W -r $PROBE 2>&1; echo "[rc=$?]"
printf 'set -u\nf() { echo $nope; echo in-f; }\nf\necho reached\n' > $PROBE
$W -r $PROBE 2>&1; echo "[rc=$?]"
printf ': ${zz?sourced}\necho reached\n' > $PROBE.inner
printf 'echo before\n. %s\necho after-source\n' $PROBE.inner > $PROBE
$W -r $PROBE 2>&1; echo "[rc=$?]"
echo ': ${zz?stdin}; echo reached' | $W -r 2>&1; echo "[rc=$?]"
rm -f $PROBE $PROBE.inner
