#!/bin/bash
# --json runs the command the normal way and prints one ASCII line of JSON
# with its status, stdout and stderr captured apart; the exit status is the
# command's.

W=$BASH
T=_run_tmp/json_run
rm -rf "$T"
mkdir -p "$T"

json() {
    "$W" --json "$@" | sed 's/"duration_ms":[0-9]*}/"duration_ms":N}/'
    echo "exit ${PIPESTATUS[0]}"
}

echo "=== status, stdout and stderr apart"
json -c 'echo hi; echo oops >&2; exit 3'
json -c 'true'
json -c 'printf "no newline"'

echo "=== the command runs as a top-level script"
json -c 'trap "echo trap ran" EXIT; exit 300'
json -e -c 'false; echo not reached'
json -c 'f() { return 9; }; f'
json -c 'echo "0=$0 1=$1 #=$#"' name one two
json -c 'exec 2>&1; echo merged >&2'

echo "=== escaping"
json -c 'printf "q\" b\\\\ t\t cr\r nl\n"'
json -c 'printf "\001\037\177"'
json -c 'printf "\305\276 \346\227\245 \360\237\216\211"'
json -c 'printf "bad\377byte \342\202 cut"'
json -c 'printf "nul\0inside"'

echo "=== with --ec, a script file and a missing file"
json --ec ZWNobyAiaGkgJDEiOyBlY2hvIGVyciA+JjI= name arg
printf 'echo "from file $1"\nexit 5\n' > "$T/s.sh"
json "$T/s.sh" one
json "$T/missing.sh"

echo "=== output of external commands is captured too"
json -c '"$0" -c "echo child out; echo child err >&2"' "$W"
