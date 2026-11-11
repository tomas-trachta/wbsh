#!/bin/bash
# EXIT/ERR/RETURN traps, set -e, pipefail and PIPESTATUS.
( trap 'echo exit-trap $?' EXIT; exit 3 ); echo "sub $?"
( set -e; false; echo not-reached ); echo "set-e $?"
( set -e; false || true; echo reached ); echo "or $?"
( set -e; if false; then :; fi; echo if-ok )
false | true; echo "pipe $?"
set -o pipefail; false | true; echo "pipefail $?"; set +o pipefail
true | false | true; echo "${PIPESTATUS[@]}"
( trap 'echo err-trap' ERR; false; true )
f() { trap 'echo ret-trap' RETURN; return 0; }; f
( set -u; echo "${undefined_x:-ok}" )
( set -u; echo "$undefined_y" ) 2>/dev/null; echo "nounset $?"
x=$(exit 5); echo "cs $?"
