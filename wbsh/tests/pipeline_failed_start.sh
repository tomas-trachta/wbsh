#!/bin/bash
# A pipeline element that cannot start (not found, a failed expansion)
# gets its own status and the other elements still run, as in bash.

nosuchcmd_xyz 2>/dev/null | cat; echo "1 ${PIPESTATUS[*]}"
echo a | nosuchcmd_xyz 2>/dev/null | wc -l; echo "2 ${PIPESTATUS[*]}"
echo "x $((1/0))" 2>/dev/null | cat; echo "3 ${PIPESTATUS[*]}"
echo a | /no/such/path 2>/dev/null | cat; echo "4 ${PIPESTATUS[*]}"
(set -o pipefail; nosuchcmd_xyz 2>/dev/null | cat; echo "5 pipefail $?")
true | false | true; echo "6 ${PIPESTATUS[*]} $?"
