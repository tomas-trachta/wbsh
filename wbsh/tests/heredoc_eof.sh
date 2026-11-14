#!/bin/bash
# A here-document that the end of the script cuts off is used up to there,
# with bash's warning, and the script goes on; it is not a syntax error.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
S=_run_tmp/heredoc_eof_probe.sh

echo "=== 1. the body runs to the end of the file"
printf 'echo before\ncat <<EOF\nnever closed\nlast line\n' > $S
$W -r $S 2>&1
echo "rc=$?"

echo "=== 2. no final newline; a quoted delimiter"
printf "cat <<'END'\n\$keep" > $S
$W -r $S 2>&1
echo "rc=$?"

echo "=== 3. in -c and eval"
$W -r -c 'cat <<EOF
body' 2>&1
echo "rc=$?"
eval 'cat <<EOF
from eval' 2>&1
echo "rc=$?"
