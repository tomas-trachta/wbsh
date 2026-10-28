#!/bin/bash
# Pipeline stages and spawned subshells whose source passes the 32767
# character Windows command-line limit still run.

big=$(printf '%040000d' 0 | tr 0 x)
echo "big=${#big}"

echo "=== 1. a stage with a 40000-character literal"
eval "{ echo \"$big\"; } | wc -c"

echo "=== 2. a heredoc body of 80000 characters"
eval "cat <<EOF | wc -c
$big$big
EOF"

echo "=== 3. a function stage with a long positional"
count() { printf '%s' "$1" | wc -c; }
set -- "$big"
count "$1" | cat
echo "$1" | count "$(cat)"
