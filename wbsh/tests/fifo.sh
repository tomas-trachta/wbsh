#!/bin/bash
# mkfifo / mknod p: FIFOs as marker files that wbsh joins through Windows
# named pipes, for redirections and the bundled commands.

if [ -x ../../x64/Release/wbsh.exe ]; then
	W=../../x64/Release/wbsh.exe
else
	W=../../x64/Debug/wbsh.exe
fi

W=$PWD/$W
T=_fifo_tmp
rm -rf $T
mkdir -p $T
cd $T

echo "=== making"
mkfifo p && echo "mkfifo made p"
mkfifo p; echo "status $?"
mknod q p && echo "mknod made q"
mknod d c 1 3; echo "status $?"
mknod e p 1 2; echo "status $?"
mknod f x; echo "status $?"
mknod; echo "status $?"
mkfifo; echo "status $?"
mkfifo -m bogus z; echo "status $?"

echo "=== what it looks like"
[ -p p ] && echo "-p fifo"
[ -e p ] && echo "-e fifo"
[ -f p ] || echo "! -f fifo"
[ -s p ] || echo "! -s fifo"
: > plain
[ -p plain ] || echo "! -p regular file"
ls -l p | cut -c1-10
ls -F p q plain
stat -c '%F %A %f %s' p
find . -type p | sort

echo "=== writer first"
seq 1 3 > p &
cat < p
wait

echo "=== reader first"
( sleep 0.2; printf 'a\nb\n' > p ) &
while read -r line; do echo "got $line"; done < p
wait

echo "=== bundled commands open it"
seq 1 1000 > p &
wc -l p
wait
seq 1 200000 > p &
tail -1 p
wait
cat p > out.txt &
echo through-tee | tee p > /dev/null
wait
cat out.txt

echo "=== external programs on the other end"
printf 'x y z\n' > p &
findstr . < p
wait

echo "=== a pipeline stage writes"
seq 1 5 | sed 's/^/n/' > p &
cat p | tr '\n' ' '; echo
wait

echo "=== held open with <>"
exec 3<> p
( echo first > p; echo second > p ) &
read -r a <&3
read -r b <&3
echo "kept: $a $b"
wait
exec 3<&-
( echo after-close > p ) &
cat p
wait

echo "=== two FIFOs do not mix"
mkdir sub
mkfifo sub/p
( echo one > p ) &
( echo two > sub/p ) &
cat sub/p
cat p
wait

echo "=== modes"
mkfifo -m 444 ro
echo x > ro; echo "status $?"
mkfifo -m u=rw,go=r rw && [ -p rw ] && echo "symbolic mode accepted"

echo "=== Ctrl+C ends a wait"
$W -r -c '__ctrlc 300; cat < p; echo not reached'; echo "reader status $?"
$W -r -c '__ctrlc 300; echo x > p; echo not reached'; echo "writer status $?"

echo "=== rm"
rm p q ro rw
ls

cd ..
rm -rf $T
