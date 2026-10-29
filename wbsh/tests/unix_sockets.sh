#!/bin/bash
# AF_UNIX socket files (reparse tag IO_REPARSE_TAG_AF_UNIX) as test -S,
# ls, stat and find see them. `__mksock` leaves one behind, as a server
# that has bound a socket does.

T=_unix_sockets_tmp
rm -rf $T
mkdir -p $T
cd $T

__mksock agent.sock
: > plain.txt
mkdir dir
ln -s agent.sock link.sock
ln -s link.sock link2.sock
ln -s missing dangling

echo "=== test"
[ -S agent.sock ] && echo "-S socket"
[ -S link.sock ] && echo "-S through a link"
[ -S link2.sock ] && echo "-S through two links"
[ -S plain.txt ] || echo "! -S regular file"
[ -S dir ] || echo "! -S directory"
[ -S dangling ] || echo "! -S dangling link"
[ -S no-such ] || echo "! -S missing"
[ -e agent.sock ] && echo "-e socket"
[ -f agent.sock ] || echo "! -f socket"
[ -f link.sock ] || echo "! -f link to socket"
[ -f plain.txt ] && echo "-f regular file"
[ -h link.sock ] && echo "-h link"

echo "=== ls"
ls -l agent.sock | cut -c1-10
ls -F
ls -p
ls --color=always agent.sock | od -c | head -2

echo "=== stat"
stat -c '%F %A %f %s' agent.sock plain.txt

echo "=== find"
find . -type s
find . -type f
find . -printf '%y %p\n' | sort

echo "=== waiting for a socket"
( sleep 0.3; __mksock late.sock ) &
until [ -S late.sock ]; do sleep 0.05; done
echo "late.sock appeared"
wait

echo "=== hook usage"
__mksock; echo "status $?"

cd ..
rm -rf $T
