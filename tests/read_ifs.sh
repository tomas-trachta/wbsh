#!/bin/bash
# Probe: `read` and IFS interactions.

echo "=== 1. read into multiple vars (default IFS)"
read a b c <<< "alpha beta gamma"
echo "a=$a b=$b c=$c"

echo "=== 2. last var soaks up the rest"
read a b rest <<< "x y z w v"
echo "a=$a b=$b rest=$rest"

echo "=== 3. IFS=: for colon-separated fields"
IFS=':' read u p uid gid info home shell <<< "alice:x:1000:1000:Alice:/home/alice:/bin/bash"
echo "u=$u uid=$uid home=$home shell=$shell"

echo "=== 4. read -r preserves backslashes; without -r they're consumed"
echo 'a\tb\nc' | { read    plain;     echo "no-r: [$plain]"; }
echo 'a\tb\nc' | { read -r raw;       echo "yes-r: [$raw]"; }

echo "=== 5. read -a array"
read -a arr <<< "one two three four"
echo "len=${#arr[@]} 0=${arr[0]} 3=${arr[3]}"

echo "=== 6. read with leading/trailing whitespace"
read a b <<< "   spaced    out   trailing   "
echo "a=[$a] b=[$b]"

echo "=== 7. while read line < file (no subshell, vars survive)"
mkdir -p _run_tmp
cat > _run_tmp/lines.txt <<'EOF'
one
two
three
EOF
count=0
while IFS= read -r line; do
    count=$((count + 1))
    last=$line
done < _run_tmp/lines.txt
echo "count=$count last=$last (bash expects 3 / three)"
rm -rf _run_tmp

echo "=== 8. IFS= read -r preserves leading/trailing whitespace"
IFS= read -r raw <<< "  preserved  "
echo "raw=[$raw]"

echo "=== done"
