#!/bin/bash
# Probe: the exit status of command substitutions in $? and set -e (bash 5.2 semantics).

echo "=== 1. a command with no name returns the last \$(...) status"
x=$(exit 3); echo "$?"
x=$(exit 3) y=$(exit 4); echo "$?"
x=$(exit 3) y=foo; echo "$?"
x=$(exit 3); y=1; echo "$?"
$(exit 5); echo "$?"
x=$(( $(exit 3) + 1 )); echo "$? $x"
x=$(exit 300); echo "$?"
false; x=$(true); echo "$?"
false; x=1; echo "$?"
false; x=$(echo $?); echo "inner sees $x"
a=($(exit 6)); echo "$?"
a[1]=$(exit 7); echo "$?"
x=`exit 8`; echo "$?"
x="$(exit 3)"; echo "$?"
x=$(exit 3) > /dev/null; echo "$?"
> /dev/null $(exit 9); echo "$?"
x=$(exit 3) $(true); echo "words expand before assignments: $?"
x=$(exit 2; echo hi); echo "$? [$x]"
x=$(trap 'exit 4' EXIT; true); echo "EXIT trap: $?"
unset y; x=${y:-$(echo hi; exit 3)}; echo "$? $x"
x=<(exit 3); echo "process substitution: $?"

echo "=== 2. \$? follows each substitution at once"
a=$(exit 3) b=$?; echo "$b"
false; echo "$(exit 3) $?"
cat <<EOF
heredoc $(exit 4) $?
EOF

echo "=== 3. a command name keeps its own status"
f() { local l=$(exit 3); echo "local: $?"; }; f
export e=$(exit 3); echo "export: $?"
declare d=$(exit 3); echo "declare: $?"
x=$(exit 3) true; echo "prefix: $?"

echo "=== 4. conditions"
if x=$(false); then echo yes; else echo no; fi
x=$(false) || echo or
x=$(false) && echo and || echo or
while x=$(exit 1); do echo never; done; echo "while: done"

echo "=== 5. set -e"
(set -e; y=$(false); echo not-reached); echo "rc=$?"
(set -e; $(exit 4); echo not-reached); echo "rc=$?"
(set -e; y=$(false) || true; echo survived); echo "rc=$?"
(set -e; g() { local v=$(false); echo "local survives"; }; g)
(set -e; x=$(true); echo ok)
(set -e; if y=$(false); then :; fi; echo "condition ok")
