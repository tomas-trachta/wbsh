#!/bin/bash
# uname, OSTYPE and MSYSTEM answer the way Git Bash does, so scripts that
# detect Windows with `case $(uname -s) in MINGW*|MSYS*)` take that branch.
# The Windows version and build differ per machine and are masked; MSYSTEM
# is pinned so the run does not depend on the shell it was started from.

export MSYSTEM=MINGW64

mask() {
    sed 's/_NT-[0-9][0-9]*\.[0-9][0-9]*-[0-9][0-9]*/_NT-<ver>-<build>/'
}

echo "=== kernel name"
uname | mask
uname -s | mask
uname --kernel-name | mask
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*) echo "detected as Windows" ;;
    *)                    echo "NOT detected: $(uname -s)" ;;
esac

echo "=== operating system, machine, processor, platform"
uname -o
case $(uname -m) in
    x86_64|aarch64) echo "machine ok" ;;
    *)              echo "machine UNEXPECTED: $(uname -m)" ;;
esac
uname -p
uname -i
uname --processor --hardware-platform

echo "=== -a"
set -- $(uname -a)
echo "fields: $#"
[ "$1" = "$(uname -s)" ] && echo "starts with uname -s"
[ "$2" = "$(uname -n)" ] && echo "then the node name"
[ "$5" = "$(uname -m)" ] && echo "then the machine"
eval "last=\${$#}"
echo "ends with $last"

echo "=== WBSH_UNAME_S"
WBSH_UNAME_S=Linux uname -s
WBSH_UNAME_S=Linux uname -a | cut -d' ' -f1
WBSH_UNAME_S= uname -s | mask

echo "=== MSYSTEM picks the prefix"
MSYSTEM=MINGW64 uname -s | mask
MSYSTEM=UCRT64 uname -s | mask
MSYSTEM=CLANG64 uname -s | mask
MSYSTEM=MINGW32 uname -s | mask
MSYSTEM=MSYS uname -s | mask

echo "=== OSTYPE and MSYSTEM"
echo "OSTYPE=$OSTYPE"
env -u MSYSTEM "$BASH" -c 'echo "default MSYSTEM=$MSYSTEM"; env | grep -c "^MSYSTEM="'
env MSYSTEM=UCRT64 "$BASH" -c 'echo "inherited MSYSTEM=$MSYSTEM"'

echo "=== /proc/version"
env -u MSYSTEM -u WBSH_UNAME_S "$BASH" -c 'cut -d" " -f1-2 /proc/version' | mask
env WBSH_UNAME_S=Linux "$BASH" -c 'cut -d" " -f1-2 /proc/version'
grep -c Microsoft /proc/version

echo "=== done"
