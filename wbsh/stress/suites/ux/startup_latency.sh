#!/bin/bash
# Start-up time people feel: a new shell, a new shell with a big environment,
# a new interactive shell, and Tab completion. Medians of seven starts.
# @stress timeout=300
. "$STRESS_ROOT/lib/check.sh"

median_ms() {
    local i start t list=""
    for i in 1 2 3 4 5 6 7; do
        start=$EPOCHREALTIME
        "$@" > /dev/null 2>&1 < /dev/null
        t=$(( (${EPOCHREALTIME/./} - ${start/./}) / 1000 ))
        list="$list$t"$'\n'
    done
    printf '%s' "$list" | sort -n | sed -n 4p
}

severity medium
t=$(median_ms "$STRESS_WBSH" -r -c :)
note "wbsh -c : median ${t} ms"
check "non-interactive start under 100 ms (${t} ms)" [ "$t" -lt 100 ]

big=$(for i in $(seq 1 300); do printf '/c/some/long/path/number%d:' "$i"; done)
t=$(median_ms env "PATH=$big$PATH" "BIGVAR=$(printf 'x%.0s' {1..30000})" "$STRESS_WBSH" -r -c :)
note "with a 300-entry PATH and a 30 kB variable: median ${t} ms"
check "start with a big environment under 150 ms (${t} ms)" [ "$t" -lt 150 ]

t=$(median_ms "$STRESS_WBSH" -i -c exit)
note "wbsh -i -c exit median ${t} ms"
check "interactive start under 250 ms (${t} ms)" [ "$t" -lt 250 ]

start=$EPOCHREALTIME
for i in $(seq 1 50); do
    __lineedit 'ech' '<TAB>' > /dev/null
done
t=$(( (${EPOCHREALTIME/./} - ${start/./}) / 50000 ))
note "command-name completion: ${t} ms per Tab"
check "command-name completion under 50 ms (${t} ms)" [ "$t" -lt 50 ]
finish
