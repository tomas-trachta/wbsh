#!/bin/bash
# A shell string cannot hold a NUL byte: command substitution drops them
# with a warning, as bash does, so the lengths agree with bash.

x=$(printf 'a\0b\0c')
echo "1 ${#x} $x"
x=`printf 'a\0b'`
echo "2 ${#x} $x"
echo "3 $(printf 'p\0q')" | od -An -c
D=$(mktemp -d) || exit 1
printf 'r\0s' > "$D/nul.bin"
x=$(< "$D/nul.bin")
echo "4 ${#x} $x"
x=$(printf 'no nul')
echo "5 ${#x} $x"
rm -rf "$D"
