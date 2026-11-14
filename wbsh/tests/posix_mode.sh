#!/bin/bash
# set -o posix (and sh.exe): the parts of bash's POSIX mode wbsh follows.
# Each probe runs in a child shell, since many of them end it.

WBSH=$(cd ../../x64/Release 2>/dev/null && pwd)/wbsh.exe
[ -x "$WBSH" ] || WBSH=wbsh

probe() {
    local out
    out=$("$WBSH" -c "set -o posix; $1"$'\n''echo "  reached, status $?"' 2>&1)
    printf '%s\n  => %s\n' "$1" "${out//$'\n'/ | }"
}

echo "=== what set -o posix turns on"
probe 'echo "POSIXLY_CORRECT=$POSIXLY_CORRECT"; shopt -p expand_aliases inherit_errexit shift_verbose'
probe 'set +o posix; echo "${POSIXLY_CORRECT-unset}"; shopt -p expand_aliases inherit_errexit shift_verbose'
"$WBSH" -c 'POSIXLY_CORRECT=1; [[ -o posix ]] && echo on; unset POSIXLY_CORRECT; [[ -o posix ]] || echo off'
POSIXLY_CORRECT=y "$WBSH" -c '[[ -o posix ]] && echo "on from the environment"'
probe 'alias hi="echo alias expanded"
hi'

echo "=== special builtins"
probe 'x=1 :; echo "x=$x"; declare -p x'
probe 'y=1 true; echo "y=${y-unset}"'
probe 'set() { echo function; }; echo not reached'
probe 'export() { echo function; }'
probe 'eval() { echo function; }'
probe 'f-1() { echo dash name; }; f-1'
probe 'export "a b"; echo not reached'
probe 'readonly r=1; export r=2; echo not reached'
probe 'unset "a b"; echo not reached'
probe 'set -o nosuch; echo not reached'
probe 'shift 5; echo not reached'
probe 'trap "" NOSIG; echo not reached'
probe 'eval "("; echo not reached'
probe 'command eval "("; echo "after command eval $?"'
probe '. ./_no_such_file; echo not reached'
probe 'command . ./_no_such_file; echo "after command . $?"'
probe 'eval "false"; echo "eval of false is fine $?"'
probe 'readonly r=1; r=2; echo not reached'
probe 'readonly r=1; r=2 :; echo not reached'
probe 'readonly r=1; r=2 echo not run; echo "after prefix error $?"'
probe 'echo $((1 +)); echo not reached'
probe 'echo ${x:?gone}; echo not reached'

echo "=== time"
probe 'TIMEFORMAT=timed; time; echo "status $?"'
"$WBSH" -c 'set -o posix; time' 2>&1 | sed 's/[0-9]/9/g'
"$WBSH" -c 'time' 2>&1 | sed 's/[0-9]/9/g'

echo "=== sourcing"
mkdir -p _posix_tmp
echo 'echo sourced from cwd' > _posix_tmp/cwdfile.sh
(cd _posix_tmp && "$WBSH" -c '. cwdfile.sh; echo "default mode reads the cwd $?"')
(cd _posix_tmp && "$WBSH" -c 'set -o posix; . cwdfile.sh; echo not reached' 2>&1)
(cd _posix_tmp && "$WBSH" -c 'set -o posix; . ./cwdfile.sh')
rm -rf _posix_tmp

echo "=== interactive shells do not exit"
printf 'set -o posix\nexport "a b"\necho still here\n' \
    | env -u WBSH_INIT_COMMAND "$WBSH" -i --norc 2>&1 | grep -ao 'still here\|not a valid identifier'

