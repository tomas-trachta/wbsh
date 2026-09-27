#!/bin/bash
# Non-interactive edge cases for the fzf fuzzy picker. The interactive
# picker loop itself needs a real console with keystrokes, so this only
# exercises the paths that resolve before/without opening one.

echo "=== fzf: empty stdin has no candidates, exits 1 with no output"
printf '' | fzf
echo "rc=$?"

echo "=== done"
