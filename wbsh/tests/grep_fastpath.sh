#!/bin/bash
# grep's literal fast paths (plain strings, alternations of strings, many -F
# needles) and the required-literal prefilter in front of the regex must
# select exactly what the regex alone would.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

printf '%s\n' 'ERROR disk full' 'error: lower' 'WARN low memory' 'INFO a.b' 'INFO axb' \
    'ab abc abcd' 'pipe a|b here' 'ac' 'abbbc' 'x{2}y' 'xxy' ']bc' '7 digits 42' \
    'aéx' 'ax' 'd' 'abcabcd' '' 'ERRX' > in.txt

echo "=== plain literal, -i, -c, -v"
grep ERROR in.txt
grep -i error in.txt
grep -c INFO in.txt
grep -vc a in.txt

echo "=== escaped operators are literal characters"
grep 'a\.b' in.txt
grep -E 'x\{2\}y' in.txt
grep 'a|b' in.txt

echo "=== alternations of plain strings"
grep -E 'WARN|ERRX' in.txt
grep 'disk\|memory' in.txt
grep -ciE 'error|warn' in.txt
grep -cE 'ERROR|' in.txt

echo "=== many -F needles: leftmost-longest -o, -w, -x"
grep -oF -e ab -e abc -e abcd in.txt
grep -wF -e ab -e abc -e bc in.txt
grep -xF -e ac -e d -e ab in.txt
grep -xF -e '' in.txt | wc -l
grep -cF -e abc -e '' in.txt
grep --color=always -F -e ab -e abcd in.txt | head -1 | od -c | head -3

echo "=== single needle with -w retries later occurrences"
echo 'abc ab' | grep -ow ab
echo 'xab abx' | grep -w ab
echo "rc=$?"

echo "=== regexes behind the required-literal prefilter"
grep -E 'ERR(OR|X)' in.txt
grep 'ab*c' in.txt
grep 'x\{0\}y' in.txt
grep '[]a]bc' in.txt
grep '[[:digit:]][[:digit:]]' in.txt
grep -E '(abc)*d' in.txt
grep -E 'a{0}c' in.txt
grep 'aé*x' in.txt
grep -i 'ERROR:' in.txt
grep -c 'a\(b\|x\)' in.txt

echo "=== several regex patterns at once"
grep -E -e 'disk f.ll' -e 'lo[wx] mem' in.txt
grep -c -e 'ab*c' -e '^$' in.txt
grep -xE -e 'a|ab' -e 'x+y' in.txt
grep -x -e 'ab*c' in.txt

echo "=== pattern file with literal and regex lines"
printf '%s\n' 'disk' 'lo.' > pats.txt
grep -f pats.txt in.txt
printf '%s\n' 'disk' 'memory' > lits.txt
grep -nf lits.txt in.txt

echo "=== 300 fixed needles"
i=0
while [ $i -lt 300 ]; do echo "needle$i"; i=$((i + 1)); done > many.txt
printf 'hay needle299 hay\nhay needle30\nnothing\nneedle7x\n' | grep -cf many.txt
printf 'hay needle299 hay\nneedle30 ok\n' | grep -of many.txt
printf 'needle1 needle10\n' | grep -owf many.txt

cd / && rm -rf "$D"
