#!/bin/bash
# The emulated /proc: system files, /proc/self and /proc/<pid>, for
# redirections, globs, test and the bundled commands. Values differ per
# machine, so only their shape and how they relate are checked.

T=_procfs_tmp
rm -rf $T
mkdir -p $T

echo "=== system files"
[ "$(grep -c ^processor /proc/cpuinfo)" = "$(nproc)" ] && echo "one cpuinfo block per processor"
grep -m1 '^model name' /proc/cpuinfo | grep -q ': .' && echo "model name present"
grep -m1 '^flags' /proc/cpuinfo | grep -qw sse2 && echo "flags list sse2"
cut -d: -f1 /proc/meminfo | tr '\n' ' '; echo
awk '/^MemTotal:/ { exit !($2 > 0 && $3 == "kB") }' /proc/meminfo && echo "MemTotal in kB"
grep -Eq '^[0-9]+\.[0-9]{2} [0-9]+\.[0-9]{2}$' /proc/uptime && echo "uptime shape"
grep -Eq '^0\.00 0\.00 0\.00 1/[0-9]+ [0-9]+$' /proc/loadavg && echo "loadavg shape"
grep -q '^wbsh version [0-9.]* (wbsh ' /proc/version && echo "version shape"
grep -qi microsoft /proc/version || echo "version does not look like WSL"
wc -l < /proc/uptime

echo "=== self"
readlink /proc/self > $T/self
[ "$(cat $T/self)" = "$$" ] && echo "self is \$\$"
readlink /proc/self/exe > $T/exe
case $(cat $T/exe) in */wbsh.exe) echo "exe is wbsh.exe" ;; esac
readlink /proc/self/cwd > $T/cwd
[ "$(cat $T/cwd)" = "$PWD" ] && echo "cwd is \$PWD"
cat /proc/$$/comm
grep '^Pid:' /proc/$$/status | cut -f2 > $T/pid
[ "$(cat $T/pid)" = "$$" ] && echo "status Pid is \$\$"
grep -c '^PPid:' /proc/self/status
tr '\0' '\n' < /proc/self/cmdline | head -1 | grep -q 'wbsh.exe$' && echo "cmdline starts with wbsh.exe"
tr '\0' '\n' < /proc/self/environ | grep -q '^PATH=\|^Path=' && echo "environ has PATH"
ls /proc/self

echo "=== fd"
# Pipeline stages and $(...) run in a child wbsh, whose /proc/self is
# its own, so the shell's descriptors are listed into a file.
exec 7< /proc/version
ls /proc/self/fd > $T/fds
grep -qx 7 $T/fds && echo "fd 7 listed"
readlink /proc/self/fd/7 > $T/fd7
grep -q '^pipe:\[[0-9]*\]$' $T/fd7 && echo "fd 7 is a pipe"
exec 7<&-
ls /proc/self/fd > $T/fds
grep -qx 7 $T/fds || echo "fd 7 gone"
exec 8> $T/out.txt
echo through-fd >&8
readlink /proc/self/fd/8 > $T/fd8
grep -q "/$T/out.txt\$" $T/fd8 && echo "fd 8 is the file"
cat /proc/self/fd/8
exec 8>&-
head -c 13 /proc/self/fd/0 < /proc/version; echo

echo "=== test"
test -e /proc/cpuinfo && echo "-e file"
test -f /proc/meminfo && echo "-f file"
test -d /proc && echo "-d /proc"
test -d /proc/self && echo "-d self"
test -L /proc/self && echo "-L self"
test -L /proc/cpuinfo || echo "! -L file"
test -s /proc/cpuinfo || echo "! -s (size reads 0)"
test -e /proc/no-such || echo "! -e missing"
test -e /proc/0 || echo "! -e pid 0"
test -d /proc/self/cwd && echo "-d cwd (outside)"

echo "=== errors"
cat /proc/no-such; echo "status $?"
cat < /proc/no-such; echo "status $?"
echo x > /proc/cpuinfo; echo "status $?"
cat /proc; echo "status $?"
readlink /proc/cpuinfo; echo "status $?"
ls /proc/no-such; echo "status $?"

echo "=== listing and globs"
ls /proc | grep -v '^[0-9]*$'
ls /proc | grep -q "^$$\$" && echo "own pid listed"
echo /proc/s*
echo /proc/self/c*
for p in /proc/[0-9]*/comm; do :; done; [ -n "$p" ] && echo "pid glob expands"
realpath /proc/self/../self/./exe > $T/real
[ "$(cat $T/real)" = "$(cat $T/exe)" ] && echo "realpath folds . and .."
readlink -f /proc/self > $T/canon
[ "$(cat $T/canon)" = "/proc/$$" ] && echo "readlink -f self"

echo "=== grep reads pipes in full"
grep -c foo <(echo foo; echo foo)

rm -rf $T
