#!/bin/bash
# A stray token in an array literal discards only the command it breaks
# (the whole line, even its earlier commands); the script reads on from the
# next line with $? at 1. In eval and source the same happens.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe
mkdir -p _run_tmp

printf '%s\n' 'echo before' 'echo same; test=(first & second); echo dropped' \
    'echo "after: $?"' > _run_tmp/array_error.sh
"$WBSH" _run_tmp/array_error.sh 2>&1 | sed 's#^.*array_error.sh:#script:#'
echo "status: ${PIPESTATUS[0]}"

printf '%s\n' 'if true; then' '  echo in-if' '  t2=(a ; b)' '  echo in-if-after' \
    'echo "next: $?"' > _run_tmp/array_error2.sh
"$WBSH" _run_tmp/array_error2.sh 2>&1 | sed 's#^.*array_error2.sh:#script:#'
echo "status: ${PIPESTATUS[0]}"

"$WBSH" -c 'eval "a=(x & y)"; echo "after eval: $?"' 2>&1

printf '%s\n' 'echo s1' 'a=(x | y)' 'echo "s2: $?"' > _run_tmp/array_error.inc
"$WBSH" -c '. _run_tmp/array_error.inc; echo "after source: $?"' 2>&1 \
    | sed 's#^.*array_error.inc:#source:#'

rm -f _run_tmp/array_error.sh _run_tmp/array_error2.sh _run_tmp/array_error.inc
