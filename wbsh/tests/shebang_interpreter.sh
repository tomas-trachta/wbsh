#!/bin/bash
# A script whose `#!` line names an interpreter that is not a shell runs
# through that interpreter, with the words after it, the script and its
# arguments (#266). The words are split at blanks, as macOS does.

mkdir -p _run_tmp/shi
dir=$(cd _run_tmp/shi && pwd)
cd "$dir"

printf '#!/usr/bin/env awk -f\nBEGIN { print "awk got", ARGC - 1, "args" }\n' > by_env
printf '#!/usr/bin/awk -f\nBEGIN { print "absolute awk" }\n' > by_path
printf '#!/usr/bin/env -S awk -v x=split -f\nBEGIN { print "env -S:", x }\n' > by_split
printf '#!/bin/cat\nthis line is printed\n' > by_cat
printf '#!/bin/echo hello\n' > by_echo
printf '#!/usr/bin/env awk -f\r\nBEGIN { print "CRLF line" }\r\n' > by_crlf
printf '#!/bin/sh\necho "interp got: $*"\n' > interp
printf '#!%s -x\n' "$dir/interp" > by_script
printf '#!/usr/bin/no-such-interpreter\necho never\n' > missing
chmod +x by_env by_path by_split by_cat by_echo by_crlf interp by_script missing

echo "=== env, absolute path, env -S"
./by_env a b
./by_path
./by_split

echo "=== cat and echo get the script path"
./by_cat
./by_echo one "two words"

echo "=== CRLF first line"
./by_crlf | tr -d '\r'

echo "=== a script as the interpreter"
./by_script arg | sed "s|$dir/||g"

echo "=== a missing interpreter"
./missing
echo "status $?"

echo "=== in a pipeline, in the background, with a temporary variable"
./by_echo piped | tr a-z A-Z
./by_echo bg &
wait
printf '#!/usr/bin/env awk -f\nBEGIN { print "V=" ENVIRON["V"] }\n' > by_var
chmod +x by_var
V=temp ./by_var

echo "=== env -S"
env -S 'echo a  "b c"' d
env -S'echo x' y
env --split-string='echo s' t
env -S 'echo "open'
echo "status $?"

cd - > /dev/null
rm -rf _run_tmp/shi
