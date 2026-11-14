#!/bin/bash
# Double quotes inside (( )), for (( )) and $(( )) are removed, as bash
# expands the expression as if it were double-quoted.

for (( i=0; "i < 3"; i++ )); do echo "i=$i"; done
for (( ; "i > 0"; i-- )); do echo "down=$i"; done
(( "1 < 3" )) && echo "arith command"
echo "sum: $(( "1 + 2" ))"
x=4; echo "subst kept: $(( $(echo '"2"' | tr -d '"') * "x" ))"
