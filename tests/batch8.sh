#!/bin/bash
echo "=== process substitution"
diff <(printf 'one\ntwo\nthree\n') <(printf 'one\nTWO\nthree\n')
echo --
echo "lines: $(wc -l < <(seq 1 5))"

echo "=== tar create / list / extract"
mkdir -p /tmp/wbsh-tar/sub
echo "alpha"  > /tmp/wbsh-tar/a.txt
echo "beta"   > /tmp/wbsh-tar/sub/b.txt
( cd /tmp/wbsh-tar && tar -cf /tmp/wbsh-tar.tar a.txt sub )
tar -tf /tmp/wbsh-tar.tar
echo --
mkdir -p /tmp/wbsh-extracted
( cd /tmp/wbsh-extracted && tar -xf /tmp/wbsh-tar.tar )
cat /tmp/wbsh-extracted/a.txt
cat /tmp/wbsh-extracted/sub/b.txt
rm -rf /tmp/wbsh-tar /tmp/wbsh-extracted /tmp/wbsh-tar.tar

echo "=== history expansion (REPL only test via -i)"
printf 'echo first\necho second\n!!\n!1\nexit\n' | ./x64/Debug/wbsh.exe -i 2>&1 | tail -10

echo "=== done"
