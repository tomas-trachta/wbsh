#!/bin/bash
# A foreground external inherits only its three stdio handles, so a
# grandchild it leaves running cannot hold the shell's own stdout pipe
# (saved by the redirections) open and stall a downstream reader.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
P=_run_tmp/inherit_probe.sh
cat > $P <<'EOF'
cmd /c "start /b ping -n 6 127.0.0.1 >NUL 2>NUL <NUL" > _run_tmp/inherit_log.txt 2>&1 </dev/null
echo done
EOF

start=$(date +%s)
$W -r $P | cat
elapsed=$(( $(date +%s) - start ))
if [ $elapsed -lt 4 ]; then
	echo "reader finished without waiting for the grandchild"
else
	echo "reader waited ${elapsed}s for the grandchild"
fi

echo "=== stdio still reaches the external"
echo piped | cmd /c "findstr piped"
cmd /c "echo to-stderr 1>&2" 2>&1 | tr -d "\r "
cmd /c "echo closed-stdin" <&- | tr -d "\r"
