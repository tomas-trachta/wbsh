#!/bin/bash
# The words of a pipeline stage are expanded in that stage's subshell:
# a `$(cat)` there reads the pipe, not the shell's own stdin, and
# assignments or `++` inside them leave the shell's variables alone.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
P=_run_tmp/stage_expansion_probe.sh
cat > $P <<'EOF'
count() { printf '%s' "$1" | wc -c; }
echo abc | count "$(cat)"
read -r rest
echo "shell stdin still has [$rest]"
EOF

echo "=== 1. \$(cat) in a function stage"
printf 'line1\nline2\n' | $W -r $P

echo "=== 2. external stages do not run side effects in the shell"
i=0
echo | cmd /c rem "$((i++))"
echo "i=$i"
unset x
echo | cmd /c rem "${x:=set}"
echo "x=[$x]"
echo | cmd /c rem "$(echo sub > _run_tmp/stage_side_effect)"
[ -f _run_tmp/stage_side_effect ] && echo "the substitution ran in the stage"
rm -f _run_tmp/stage_side_effect

echo "=== 3. plain words still launch the external directly"
echo hello | findstr hello
y=word
echo "$y" | findstr "$y"
