#!/bin/bash
# A syntax error stops a script, -c string, eval or source where bash
# would: the complete lines before it run, then status 2.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
S=_run_tmp/syntax_probe.sh

run_probe() {
	$W -r $S 2>&1
	echo "rc=$?"
}

echo "=== 1. missing done: only the line before runs"
printf 'echo before\nfor x in a b; do echo $x\necho after\n' > $S
run_probe

echo "=== 2. stray tokens are errors"
printf 'echo a\n)\necho b\n' > $S
run_probe
printf 'echo a\nfi\necho b\n' > $S
run_probe
printf 'echo a\necho b; done; echo c\necho d\n' > $S
run_probe

echo "=== 3. the whole line with the error is skipped"
printf 'echo a; if\n' > $S
run_probe
printf 'f() { echo in f\n}\nf\nif true; then echo t; fi; case\n' > $S
run_probe

echo "=== 4. lexer errors"
printf 'echo a\necho "unterminated\necho c\n' > $S
run_probe
printf 'echo a\necho $(case x in\n' > $S
run_probe

echo "=== 5. exit before the error wins; the EXIT trap sees 2"
printf 'echo a\nexit 3\nfi\n' > $S
run_probe
printf 'trap "echo trap \\$?" EXIT\necho a\nfi\n' > $S
run_probe

echo "=== 6. -c"
$W -r -c 'echo a
fi
echo b' 2>&1
echo "rc=$?"

echo "=== 7. eval and source return 2 and the shell goes on"
eval "echo x; fi" 2>&1
echo "eval rc=$?"
eval "echo y
fi
echo z" 2>&1
echo "eval rc=$?"
printf 'echo sourced\nfi\necho not reached\n' > $S
. $S 2>&1
echo "source rc=$?"
