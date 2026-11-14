#!/bin/bash
# Probe: errexit inside $(...) follows inherit_errexit and posix mode, as in bash.

echo "=== 1. set -e is off inside \$(...) by default"
(set -e; x=$(false; echo "went on"); echo "x=[$x]")
(set -e; f() { false; echo "in f"; }; y=$(f); echo "y=[$y]")
(set -e; z=$(echo "opts: $-"); echo "$z")

echo "=== 2. set -e inside \$(...) applies there"
(set -e; w=$(set -e; false; echo "not reached"); echo "not reached either"); echo "rc=$?"

echo "=== 3. inherit_errexit keeps it"
(set -e; shopt -s inherit_errexit; x=$(false; echo "not reached"); echo "not reached either"); echo "rc=$?"
(set -e; shopt -s inherit_errexit; x=$(false; echo "guarded") || :; echo "x=[$x]")

echo "=== 4. so does posix mode"
(set -e -o posix; x=$(false; echo "not reached"); echo "not reached either"); echo "rc=$?"

echo "=== 5. the outer shell keeps errexit"
(set -e; x=$(true); false; echo "not reached"); echo "rc=$?"
