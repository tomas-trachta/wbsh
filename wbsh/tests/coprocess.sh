#!/bin/bash
# coproc [NAME] command: NAME[0] reads the coprocess output, NAME[1] writes
# its input, NAME_PID is its process. The probe runs under a timeout.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
P=_run_tmp/coprocess_probe.sh
cat > $P <<'EOF'
coproc cat
echo hi >&${COPROC[1]}; read l <&${COPROC[0]}; echo $l
echo "pid set: $([ -n "$COPROC_PID" ] && echo yes)"
[ "${COPROC[0]}" -ge 10 ] && [ "${COPROC[1]}" -ge 10 ] && echo "fds above 9"
echo second >&${COPROC[1]}; read l <&${COPROC[0]}; echo $l
coproc UP { while read -r line; do echo "${line^^}"; done; }
echo hello >&${UP[1]}; read -r r <&${UP[0]}; echo "$r"
echo world >&${UP[1]}; read -r r <&${UP[0]}; echo "$r"
coproc SQ ( read -r n; echo $((n * n)) )
echo 7 >&${SQ[1]}; read -r r <&${SQ[0]}; echo "square $r"
wait $SQ_PID; echo "SQ exited $?"
coproc python -I -u -c "import sys; [print(l.strip()[::-1], flush=True) for l in sys.stdin]"
echo abc >&${COPROC[1]}; read -r r <&${COPROC[0]}; echo "reversed ${r%$'\r'}"
echo "${#COPROC[@]}"
coproc echo simple command name
read -r r <&${COPROC[0]}; echo "$r"
EOF

timeout 60 $W -r $P > _run_tmp/coprocess_probe.log 2>&1
echo "rc=$?"
cat _run_tmp/coprocess_probe.log
