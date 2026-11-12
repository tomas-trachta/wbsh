#!/bin/bash
# Temporary files: unique names, and no data left behind in the temp folder.
. "$STRESS_ROOT/lib/check.sh"

t=$(mktemp)
tmpdir=$(dirname "$t")
rm -f "$t"
note "temp folder: $tmpdir"

severity high
names=$(for i in $(seq 1 200); do mktemp -u; done | sort | uniq -d | wc -l)
expect_eq "200 mktemp names are unique" "$names" 0
f=$(mktemp)
check "mktemp creates the file" [ -f "$f" ]
expect_eq "mktemp file is empty" "$(wc -c < "$f")" 0
rm -f "$f"
d=$(mktemp -d)
check "mktemp -d creates a directory" [ -d "$d" ]
rmdir "$d"

marker="SECRET-$RANDOM$RANDOM$RANDOM"
: > stamp
"$STRESS_WBSH" -r -c '
m=$1
cat <<EOF > /dev/null
here-doc $m
EOF
read -r x <<< "here-string $m"
y=$(echo "subst $m")
cat <(echo "procsubst $m") > /dev/null
echo "pipe $m" | sort | cat > /dev/null
printf "%s\n" "$m" b a > data.txt
sort -o data.txt data.txt
sed -i "s/a/A/" data.txt
' _ "$marker" < /dev/null > /dev/null 2>&1
leaks=$(find "$tmpdir" -maxdepth 1 -type f -newer stamp 2>/dev/null | while read -r p; do grep -l "$marker" "$p" 2>/dev/null; done)
expect_eq "no file in the temp folder keeps here-doc / pipe data" "$leaks" ""
left=$(ls -A | grep -v -e '^stamp$' -e '^data.txt$' -e '^\.run\.')
expect_eq "sort -o and sed -i leave no temporary file in the work folder" "$left" ""
finish
