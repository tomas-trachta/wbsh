#!/bin/bash
# Pipeline stages and other subshells see the parent's traps (listed, not
# run) and its directory stack, as forked bash subshells do.

echo "=== traps"
trap ': exit trap' EXIT
trap '' QUIT
trap 'echo "it'\''s INT"' INT
trap | cat
(trap)
x=$(trap -p EXIT); echo "substituted: $x"
{ trap -p QUIT; } | cat
echo "pipeline" | { read -r w; echo "$w: $(trap -p INT)"; }

echo "=== traps are listed, not run, in a stage"
trap 'echo "EXIT trap ran in $BASHPID"' EXIT
{ echo stage; } | cat
trap - EXIT QUIT INT
trap | cat
echo "no traps left: $(trap | wc -l)"

echo "=== a DEBUG trap stays quiet while the stage is set up"
trap ': debug' DEBUG
trap ': err' ERR
trap | cat
trap - DEBUG ERR

echo "=== the directory stack"
D=$(mktemp -d) || exit 1
mkdir -p "$D/one" "$D/two"
cd "$D" || exit 1
pushd one > /dev/null
pushd ../two > /dev/null
dirs | sed "s|$D|D|g"
dirs | cat | sed "s|$D|D|g"
(dirs) | sed "s|$D|D|g"
{ dirs -v; } | sed "s|$D|D|g"
y=$(dirs); echo "${y//$D/D}"
popd > /dev/null
dirs | cat | sed "s|$D|D|g"
cd / && rm -rf "$D"
