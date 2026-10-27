#!/bin/bash
# awk strnum: input that looks numeric compares as a number, constants keep their type.

echo "=== fields compare numerically"
echo '10 9' | awk '{print ($1>$2)}'
printf '10\n9\n100\n' | awk '$1>max{max=$1} END{print max}'
printf '10\n9\n100\n' | awk 'NR==1||$1<min{min=$1} END{print min}'
echo '1e2 100' | awk '{print ($1==$2)}'
echo '3.0 3' | awk '{print ($1 == $2)}'
echo 'abc 100' | awk '{print ($1<$2)}'
echo ' 10 ' | awk -F, '{print ($1 == 10), ($1 < 9)}'
echo '010 10' | awk '{print ($1 == $2); x[$1]=1; print (10 in x), ("010" in x)}'

echo "=== what counts as numeric input"
echo '0x1A 1e3 .5 5. +5 -5 " 7" inf' | awk '{for(i=1;i<=NF;i++) printf "%s:%d ", $i, ($i == $i+0); print ""}'
echo '+inf -nan' | awk '{print ($1 == $1+0), ($2 == $2+0), $1+0}'
awk 'BEGIN{print "inf"+0, "+inf"+0, "nan"+0, "-nan"+0, "0x10"+0, " 12abc"+0, ".5"+0, "5."+0, "1e3x"+0, "e5"+0, "+-3"+0}'
echo 'a,,b' | awk -F, '{print ($2 == 0), ($5 == 0), ($5 == "")}'
echo '5' | awk '{ NF = 3; print ($3 == 0), ($3 == ""), $0 "|" }'
echo 65 | awk '{printf "%c\n", $1}'

echo "=== other strnum sources"
echo '2 10' | awk '{split($0, a); print (a[1] < a[2])}'
printf '3\n20\n' | awk '{getline n; print ($0 < n)}'
printf '3\n20\n' > _awk_strnum.txt
awk 'BEGIN{getline a < "_awk_strnum.txt"; getline b < "_awk_strnum.txt"; print (a < b)}'
rm -f _awk_strnum.txt
awk -v a=3 -v b=20 'BEGIN{print (a < b)}'
echo 'a:b' | awk -v FS=: '{print $2}'

echo "=== constants and assignments keep their type"
awk 'BEGIN{x="10"; print (x < 9)}'
awk 'BEGIN{x=10; print (x < "9")}'
echo '5' | awk '{ $2 = "10"; print ($2 < 9), NF }'
echo '5 6' | awk '{ $3 = $1 + $2; print ($3 > 10), $0 }'
echo '1 2' | awk '{ $1 = $1 + 0.5; print ($1 > 1), $0 }'
awk 'BEGIN{a[2];a[10]; for(k in a) print k, (k<5)}' | sort

echo "=== truth values"
echo 0 | awk '$1{print "yes"} !$1{print "no"}'
awk 'BEGIN{if ("0") print "string 0 true"}'
awk 'BEGIN{x = substr("0", 1); if (x) print "substr 0 true"}'
echo 'x' | awk '{print (u == 0), (u == ""), (u < 1)}'

echo "=== number output"
awk 'BEGIN{print 1e999, -1e999}'
awk 'BEGIN { x = 0.1 + 0.2; print x; y = 1e6; print y; z = 123456789; print z; print 3.0, 1e20, 0.000001234 }'
