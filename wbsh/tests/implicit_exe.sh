#!/bin/bash
# As in Git Bash, a missing NAME with NAME.exe beside it is that file for
# the tools that look at or read files: `ls "$JAVA_HOME/bin/java"` finds
# java.exe (#294). A tool that writes makes NAME itself.

mkdir -p _run_tmp/iex
dir=$(cd _run_tmp/iex && pwd)
cd "$dir"
printf 'MZ fake program\n' > tool.exe
mkdir sub

echo "=== test, ls and stat"
[ -x tool ] && echo "test -x: yes"
ls tool
ls -l tool | awk '{ print $1, $5, $NF }'
stat -c '%n %s %F' tool
ls tool.exe

echo "=== readers"
cat tool
wc -c tool
head -c 2 tool; echo
du tool
file tool | sed 's/:.*//'

echo "=== a name that is nowhere"
ls nothing > /dev/null 2>&1
echo "ls status $?"
stat nothing
echo "stat status $?"
du nothing
echo "du status $?"
cat nothing
echo "cat status $?"

echo "=== writers make NAME"
echo text > tool
cat tool
ls
cat tool.exe

echo "=== a POSIX path to a file shows its size (ls -l)"
printf 'twelve bytes' > sized
ls -l "$dir/sized" | awk '{ print $5 }'

cd - > /dev/null
rm -rf _run_tmp/iex
