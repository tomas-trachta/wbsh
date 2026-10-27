#!/bin/bash
# Probe: command-line arguments reach the script as $0 / $1.. (bash semantics).

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
PROBE=_run_tmp/args_probe
cat > $PROBE <<'EOF'
echo "0=$0 n=$# all=[$*]"
for a in "$@"; do echo "  arg=[$a]"; done
EOF

echo "=== 1. wbsh -r file args"
$W -r $PROBE foo bar
$W -r $PROBE 'with space' ''

echo "=== 2. option parsing stops at the script operand"
$W -r $PROBE -x -- -r --help

echo "=== 3. -- ends the options"
$W -r -- $PROBE a b

echo "=== 4. -c cmd name args"
$W -r -c 'echo "0=$0 n=$# all=[$*]"'
$W -r -c 'echo "0=$0 n=$# all=[$*]"' name a 'b c'
$W -r -c 'echo "0=$0 1=$1"' -x -y

echo "=== 5. stdin with arguments: - and -s"
echo 'echo "n=$# all=[$*]"' | $W -r - a b
echo 'echo "n=$# all=[$*]"' | $W -r -s c

echo "=== 6. positionals reach pipeline elements"
$W -r -c 'echo "$1" | cat; printf "%s|" "$@" | cat; echo; f() { echo "f: $0 $#"; }; f x | cat' me p 'q r'

echo "=== 7. shift and set -- work on them"
$W -r -c 'shift; echo "after shift: $*"; set -- z; echo "now: $*"' n a b

echo "=== 8. unknown options are rejected"
$W -r -z $PROBE
echo "rc=$?"

rm -f $PROBE
