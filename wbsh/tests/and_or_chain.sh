#!/bin/bash
# An && / || chain runs in a loop, not a call per stage: 20000 stages
# used to overflow the stack. set -e ignores every stage but the last.

n=${CHAIN_STAGES:-20000}
chain=$(printf 'true && %.0s' $(seq "$n"))
eval "${chain}echo long-chain-ok"
chain=$(printf 'false || %.0s' $(seq "$n"))
eval "${chain}echo long-or-chain-ok"
chain=$(printf 'true && false || %.0s' $(seq "$n"))
eval "${chain}echo mixed-chain-ok"

( set -e; false && echo no; echo "1 after $?" )
( set -e; true || false && echo "2 yes" )
( set -e; false || true && echo "3 yes" )
( set -e; f() { false && true; }; f || echo "4 f status $?" )
( set -e; x=0; false && x=1 || x=2; echo "5 x=$x" )
( set -e; true && false || echo "6 or" )
( set -e; true && false; echo "7 not reached" ); echo "7 status $?"
( set -e; false || false || true; echo "8 status $?" )
( set -e; true && { false; echo "9 in brace"; } || echo "9 brace or" )
( set -e; for i in 1 2; do true && break || echo no; done; echo "10 loop done" )
( set -e; g() { return 3 && echo no; }; g; echo "11 not reached" ); echo "11 status $?"
( set -e; false && true; echo "12 after a failed first stage" )
