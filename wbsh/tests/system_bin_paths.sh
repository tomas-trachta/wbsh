#!/bin/bash
# `/usr/bin/env`, `/bin/echo`: a command named by a Linux system path that
# does not exist here runs the builtin or PATH command of that name (#293).
# A function of that name does not take it, and an unknown name still
# reports the full path.

echo "=== builtins by path"
/usr/bin/env echo through env
/bin/echo direct
/usr/local/bin/printf '%s\n' printf
/bin/echo piped | tr a-z A-Z
/bin/echo background &
wait

echo "=== a function does not take the path"
echo() { builtin echo "the function"; }
/bin/echo not the function
unset -f echo

echo "=== other runners"
command /bin/echo by command
echo xa | xargs /bin/echo xargs
find . -maxdepth 0 -exec /bin/echo find {} \;
( exec /bin/echo by exec )

echo "=== lookup"
type /usr/bin/env
type -t /bin/echo
command -v /usr/bin/env
which /bin/echo

echo "=== a name that is nowhere"
/usr/bin/no-such-command-here
echo "status $?"
type /usr/bin/no-such-command-here
echo "status $?"
/usr/bin/sub/dir
echo "status $?"
