#!/bin/bash
# Text parsed at run time ($(...), eval, source) is freed once it has run,
# unless it defined a function, whose body must stay valid for later calls.

rss() {
    awk '/^VmRSS/ { print $2 }' /proc/self/status
}

echo "=== functions defined in parsed text still work later"
eval 'from_eval() { echo "from eval: $1"; }'
printf 'from_source() { echo "from source: $1"; }\n' > "${TMPDIR:-/tmp}/arena_lifetime_src.sh"
. "${TMPDIR:-/tmp}/arena_lifetime_src.sh"
rm -f "${TMPDIR:-/tmp}/arena_lifetime_src.sh"
x=$(in_subst() { echo "from subst"; }; in_subst)
echo "$x"
for i in $(seq 1 200); do y=$(echo "$i"); eval "z=$i"; done
from_eval "$y"
from_source "$z"
type in_subst > /dev/null 2>&1 && echo "in_subst leaked out of the subshell" || echo "in_subst stayed in the subshell"

echo "=== redefining a function from eval many times"
for i in $(seq 1 300); do eval "again() { echo again-$i; }"; done
again

echo "=== memory stays flat over 3000 command substitutions and evals"
for i in $(seq 1 300); do y=$(echo "$i"); eval "z=$i"; done
before=$(rss)
for i in $(seq 1 3000); do y=$(echo "$i"); eval "z=$i"; done
after=$(rss)
growth=$(( after - before ))
if [ "$growth" -lt 8192 ]; then echo "bounded"; else echo "grew by $growth kB"; fi
