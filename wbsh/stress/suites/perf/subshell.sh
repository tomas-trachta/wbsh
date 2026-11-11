#!/bin/bash
# ( ... ) subshells and { ...; } | builtin pipelines.
for ((i = 0; i < 500 * STRESS_N; i++)); do
    ( x=$i )
    echo "$i" | read -r y
done
echo done
