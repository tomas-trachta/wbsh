#!/bin/bash
# Integration: mini log parser. Combines heredocs, while-read, associative
# arrays, sed BRE, and arithmetic.

mkdir -p _run_tmp
cat > _run_tmp/app.log <<'EOF'
2026-05-07 09:01:00 INFO  app started
2026-05-07 09:01:01 INFO  loaded config
2026-05-07 09:02:15 WARN  cache miss for /api/users
2026-05-07 09:02:20 ERROR db connection refused
2026-05-07 09:02:21 ERROR db connection refused
2026-05-07 09:03:00 INFO  retry succeeded
2026-05-07 09:04:11 WARN  cache miss for /api/posts
2026-05-07 09:05:00 ERROR auth token expired
2026-05-07 09:05:01 INFO  reauth ok
2026-05-07 09:05:30 DEBUG heartbeat
2026-05-07 09:05:31 DEBUG heartbeat
EOF

declare -A by_level
total=0

while IFS= read -r line; do
    # BRE capture: the level token is the third whitespace-separated word.
    level=$(echo "$line" | sed 's/^[^ ]* [^ ]* \([A-Z][A-Z]*\).*$/\1/')
    by_level[$level]=$((${by_level[$level]:-0} + 1))
    total=$((total + 1))
done < _run_tmp/app.log

echo "=== totals"
echo "total=$total"

# NB: this iterates an explicit known-levels list rather than
# `for k in $(echo "${!by_level[@]}" | sort)`. The pipeline-inside-cmdsub
# form doesn't see the assoc array on wbsh today (arrays don't survive
# pipeline self-spawn) — known limitation tracked separately.
echo "=== per level (sorted)"
for k in DEBUG ERROR INFO WARN; do
    count=${by_level[$k]:-0}
    [ "$count" -gt 0 ] && printf "  %-6s %d\n" "$k" "$count"
done

echo "=== first error"
grep ERROR _run_tmp/app.log | head -1

echo "=== summary line"
errs=${by_level[ERROR]:-0}
warns=${by_level[WARN]:-0}
ok=$((total - errs - warns))
echo "$total events: $errs errors, $warns warnings, $ok other"

# Threshold check using $[ ... ] arithmetic syntax.
threshold=2
status="ok"
[ $[ $errs ] -ge $threshold ] && status="degraded"
echo "status=$status"

rm -rf _run_tmp
echo "=== done"
