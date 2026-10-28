#!/bin/bash
# <(cmd) and >(cmd) run concurrently with the command, connected through
# a named pipe: endless producers stop when the reader is done, and
# nothing is written to disk.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

mkdir -p _run_tmp
P=_run_tmp/procsubst_probe.sh
cat > $P <<'EOF'
echo "=== 1. <(...) with builtins, functions and redirections"
cat <(echo hello)
cat <(echo x) <(echo y)
diff <(printf 'a\nb\n') <(printf 'a\nc\n'); echo "diff rc=$?"
while read -r l; do echo "got $l"; done < <(printf 'one\ntwo\n')
f() { cat "$1"; }; f <(echo in-function)
x=$(cat <(echo in-substitution)); echo "$x"
cat <(cat <(echo nested))
source <(echo 'echo sourced')
[ -e <(true) ] && echo "the path exists"
cat <(true); echo "empty producer ok"
cat <(sleep 1; echo slow producer)

echo "=== 2. endless producers end with the reader"
head -n 1 <(yes); echo "head done"
head -c 5 <(yes abc); echo
wc -l < <(seq 1 100000)
echo <(yes) > /dev/null; echo "unread producer released"

echo "=== 3. external readers"
cmd /c type <(echo typed)

echo "=== 4. >(...)"
echo to-cat > >(cat); wait $!
printf 'a\nb\nc\n' > _run_tmp/procsubst_in.txt
tee >(wc -l) < _run_tmp/procsubst_in.txt > /dev/null; wait $!
: > >(cat); wait $!; echo "empty writer ok"
echo to-file > >(cat > _run_tmp/procsubst_out.txt); wait $!
cat _run_tmp/procsubst_out.txt

echo "=== 5. the word is a pipe name"
echo <(true) | sed 's/[0-9][0-9]*/N/g'
EOF

timeout 60 $W -r $P > _run_tmp/procsubst_probe.log 2>&1
echo "rc=$?"
tr -d '\r' < _run_tmp/procsubst_probe.log
