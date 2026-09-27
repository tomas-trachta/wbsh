#!/bin/bash
# Probe (integration): CSV column stats. Exercises IFS=, with read,
# arrays, arithmetic across many lines, function returns via stdout.

mkdir -p _run_tmp
cat > _run_tmp/data.csv <<'EOF'
name,score,latency_ms
alice,87,120
bob,72,145
carol,91,98
dave,64,210
eve,88,113
frank,79,160
EOF

# Skip header, parse the rest. Each iteration accumulates sums.
score_total=0
latency_total=0
count=0
max_score=0
min_latency=99999
max_score_name=""

# Discard header line.
{ read header
  while IFS=, read name score latency; do
      count=$((count + 1))
      score_total=$((score_total + score))
      latency_total=$((latency_total + latency))
      if [ "$score" -gt "$max_score" ]; then
          max_score=$score
          max_score_name=$name
      fi
      if [ "$latency" -lt "$min_latency" ]; then
          min_latency=$latency
      fi
  done
} < _run_tmp/data.csv

avg_score=$((score_total / count))
avg_latency=$((latency_total / count))

echo "header: $header"
echo "rows: $count"
echo "avg score: $avg_score"
echo "avg latency: $avg_latency ms"
echo "max score: $max_score ($max_score_name)"
echo "min latency: $min_latency ms"

# Now do it with a function that returns the avg via stdout.
avg() {
    # $1 = file, $2 = column index (1-based, comma-separated)
    awk -F, -v col="$2" 'NR>1 {s+=$col;n++} END {if(n>0) print int(s/n); else print 0}' "$1"
}
echo "avg via awk: score=$(avg _run_tmp/data.csv 2) latency=$(avg _run_tmp/data.csv 3)"

rm -rf _run_tmp
echo "=== done"
