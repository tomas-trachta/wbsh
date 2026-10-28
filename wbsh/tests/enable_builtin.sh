#!/bin/bash
# The enable builtin: list, disable and re-enable builtins.

echo "=== listing"
enable | head -3
enable -p | grep -c '^enable [^-]' | sed 's/[0-9][0-9]*/N/'
enable -s | head -4
enable -n
echo "nothing disabled: $?"

echo "=== disabling"
enable -n cd
enable -n
type -t cd
cd() { echo "function cd"; }
cd /
unset -f cd
builtin cd /; echo "builtin cd: $?"
command -v cd >/dev/null; echo "command -v cd: $?"
enable -a | grep -e 'enable -n cd$' -e 'enable cd$'
enable cd
type -t cd
enable -n

echo "=== a disabled coreutil falls back to PATH"
enable -n basename
type -t basename
[ "$(basename /a/b.txt .txt)" = b ] && echo "external basename works"
echo /x/y | xargs basename
( type -t basename )
{ type -t basename; } | cat
enable basename
type -t basename

echo "=== errors"
enable nosuch; echo "status $?"
enable -n nosuch echo; echo "status $?"
enable -n
enable echo
enable -z; echo "status $?"
