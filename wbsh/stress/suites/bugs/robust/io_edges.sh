#!/bin/bash
# Closed, binary and shared file descriptors.
# @stress timeout=120
. "$STRESS_ROOT/lib/check.sh"

wbsh_run 'exec 1>&-; echo x; echo rc=$? >&2'
expect_match "echo to a closed stdout reports failure" "$RUN_ERR" "*rc=1*"
wbsh_run 'exec 0<&-; read x; echo rc=$?'
expect_eq "read from a closed stdin fails" "$RUN_OUT" "rc=1"
wbsh_run 'printf "a\0b\0c" | od -An -c | tr -s " "'
expect_eq "NUL bytes pass a pipe unchanged" "$RUN_OUT" " a \0 b \0 c"
wbsh_run 'printf "\xff\xfe\x80abc" | wc -c'
expect_eq "invalid UTF-8 passes a pipe unchanged" "$RUN_OUT" 6
wbsh_run 'x=$(printf "a\0b" 2>/dev/null); echo ${#x}'
expect_eq "NUL is dropped from command substitution (bash: 2)" "$RUN_OUT" 2
wbsh_run 'yes | head -n 3 | wc -l'
expect_eq "yes | head ends when the reader goes away" "$RUN_OUT" 3
wbsh_run 'echo $( (sleep 30 &) ; echo quick)'
expect_eq "background job in a substitution does not hold the pipe" "$RUN_OUT" quick

: > shared.txt
wbsh_run 'for w in 1 2 3 4 5 6 7 8; do ( for i in $(seq 1 200); do echo "w$w line $i" >> shared.txt; done ) & done; wait; wc -l < shared.txt'
expect_eq "8 parallel appenders lose no line" "$RUN_OUT" 1600
wbsh_run 'grep -c "^w[1-8] line [0-9]*$" shared.txt'
expect_eq "parallel appends do not interleave inside a line" "$RUN_OUT" 1600

wbsh_run 'for i in $(seq 1 60); do sleep 0.2 & done; wait; jobs | wc -l'
expect_eq "60 background jobs are all reaped" "$RUN_OUT" 0
wbsh_run "echo x$(printf ' | cat%.0s' {1..150})"
expect_eq "150-stage pipeline" "$RUN_OUT" x
wbsh_run 'start=$SECONDS; timeout 1 sleep 20; echo "rc=$? t=$(( SECONDS - start ))"'
expect_match "timeout stops a command and returns 124" "$RUN_OUT" "rc=124 t=[012]"
wbsh_run 'for i in $(seq 1 300); do exec {fd}>"f$i"; done; echo $fd'
check "300 open descriptors" [ "$RUN_RC" -eq 0 ]
head -c 5000000 /dev/zero | tr '\0' x > big.txt
wbsh_run 'cat <<EOF | wc -c
$(cat big.txt)
EOF'
expect_eq "5 MB here-document" "$RUN_OUT" 5000001
finish
