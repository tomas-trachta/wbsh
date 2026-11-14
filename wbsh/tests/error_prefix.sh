#!/bin/bash
# The shell's own error messages carry bash's prefix: the script and line
# in a script, `wbsh: line N:` for -c (or the name given after the
# command), the sourced file inside `source`, and no line at all for
# getopts and the usage lines.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
S=_run_tmp/prefix_probe.sh

run_probe() {
	$W -r $S 2>&1
	echo "rc=$?"
}

echo "=== 1. runtime errors in a script name it and the line"
printf 'echo a\nnosuch_cmd_wbsh_x\nunset u; echo ${u?gone}\n' > $S
run_probe
printf 'cd ./no-such-dir\nsource ./no-such-file.sh\necho $(( 1 / 0 ))\n' > $S
run_probe
printf 'readonly R=1\nR=2\necho x >&7\n' > $S
run_probe

echo "=== 2. a function reports the line inside it"
printf 'f() {\n\techo $(( 2 ** -1 ))\n}\nf\n' > $S
run_probe

echo "=== 3. -c says line, and uses the name after the command"
$W -r -c 'echo a
nosuch_cmd_wbsh_x' 2>&1
echo "rc=$?"
$W -r -c 'nosuch_cmd_wbsh_x' myname 2>&1
echo "rc=$?"

echo "=== 4. a sourced file names itself"
printf 'echo in lib\nnosuch_cmd_wbsh_x\n' > _run_tmp/prefix_lib.sh
printf '. _run_tmp/prefix_lib.sh\nnosuch_cmd_wbsh_y\n' > $S
run_probe

echo "=== 5. getopts names \$0 only; usage lines have no prefix"
printf 'f() { local OPTIND o; while getopts ab: o; do :; done; }\nf -x\nf -b\nprintf\n' > $S
run_probe

echo "=== 6. arithmetic keeps bash's spacing"
printf 'echo $(( 1 / 0 ))\n(( 3 3 ))\nlet " 1 %% 0 "\necho $(( 1 ? ))\necho $(( 08 + 1 ))\n' > $S
run_probe

echo "=== 7. eval syntax errors: bash's two lines, status 2"
printf 'eval "if then"\necho "rc=$?"\n' > $S
run_probe
