#!/bin/bash
# Exit statuses and lookups as bash has them: a directory cannot run
# (126), an assignment to a readonly variable fails with 1 and the script
# goes on, `echo` to a closed stdout is a write error, a PATH= prefix is
# used to find its command, and .COM programs are found.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

echo "=== a directory is not a command"
mkdir adir
./adir 2>&1
echo "status $?"

echo "=== readonly assignments"
y=1
readonly y
y=2 2>/dev/null
echo "bare assignment: $?"
y=3 echo "prefix assignment still runs the command" 2>/dev/null
echo "y is $y"

echo "=== echo to a closed stdout"
( exec 1>&-; echo lost; echo "status $?" >&2 ) 2>&1

echo "=== a PATH= prefix finds its command"
mkdir bin
printf '#!/bin/sh\necho found through the prefix\n' > bin/only_here
chmod +x bin/only_here
PATH="$PWD/bin:$PATH" only_here
only_here 2>/dev/null
echo "without the prefix: $?"

echo "=== .COM programs are found"
type chcp > /dev/null 2>&1 && echo "chcp found" || echo "chcp NOT found"

cd / && rm -rf "$D"
