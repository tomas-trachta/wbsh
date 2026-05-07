#!/bin/bash
# Integration test mirroring the shape of a real bash exercise:
# functions + indexed arrays + IFS + glob + BRE sed + legacy $[..] arith.
# Touches every fix landed for $0 / $[ / sed-BRE in one place.

D=_run_tmp/count_lines
rm -rf $D
mkdir -p $D

# Three fixtures with known line counts, each ending in a newline so
# `wc -l` numbers are stable across platforms.
cat > $D/a.txt <<'EOF'
alpha
beta
gamma
EOF

cat > $D/b.txt <<'EOF'
one
two
EOF

cat > $D/c.txt <<'EOF'
just-one
EOF

get_files () {
    files="`ls $D/*.txt`"
}

count_lines () {
    f=$1
    # BRE capture: take leading digits from `wc -l` output and drop
    # everything else (the filename, leading spaces, etc.).
    l=`wc -l $f | sed 's/^[ ]*\([0-9][0-9]*\).*$/\1/'`
}

# Split on newlines so filenames containing spaces would still work.
IFS=$'\012'

echo "script: $0"

n=0
s=0
get_files
for f in $files
do
    count_lines $f
    file[$n]=$f
    lines[$n]=$l
    n=$[ $n + 1 ]
    s=$[ $s + $l ]
done

echo "files: $n"
echo "total: $s"

i=0
while [ $i -lt $n ]
do
    echo "  [$i] ${file[$i]} -> ${lines[$i]}"
    i=$[ $i + 1 ]
done

# Out-of-range subscripts return empty — bash semantics.
echo "oob: '${file[99]}' '${lines[99]}'"

rm -rf $D
echo "=== done"
