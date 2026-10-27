#!/bin/bash
# awk regex fast paths and caching, lazy field splitting, block reads, single-evaluation lvalues.

tmp=_awk_regex_tmp
rm -rf $tmp
mkdir -p $tmp

echo "=== literal patterns"
printf 'a.b\naxb\n^x\nx$\n' > $tmp/meta.txt
awk '/a\.b/' $tmp/meta.txt
awk '/\^x/' $tmp/meta.txt
awk '/x\$/' $tmp/meta.txt
awk '/^ax/' $tmp/meta.txt
awk '/b$/' $tmp/meta.txt
awk '/^axb$/' $tmp/meta.txt
awk '/^$/ { n++ } END { print n + 0 }' $tmp/meta.txt
printf 'one\n\ntwo\n' | awk '/^$/ { print "empty at", NR }'
printf 'tab\there\n' | awk '/\t/ { print "has tab" }'
echo 'path/to/x' | awk '/\// { n = gsub(/\//, "|"); print n, $0 }'
echo 'abc' | awk '{ print match($0, /c$/), RSTART, RLENGTH; print match($0, /^b/) }'
echo 'aaa' | awk '{ print gsub(/^a/, "X"), $0 }'
echo 'aaa' | awk '{ print gsub(/a$/, "X"), $0 }'
echo 'xyz' | awk '{ print gsub(//, "-"), $0 }'
echo 'a.b.c' | awk '{ n = split($0, p, /\./); print n, p[1] p[2] p[3] }'
echo 'hello' | awk '$0 ~ "ell" { print "dynamic literal" }'
echo 'hello' | awk '$0 ~ "^h.l" { print "dynamic regex" }'
echo 'a+b' | awk '{ sub("\\+", "plus"); print }'

echo "=== dynamic regex cache"
awk 'BEGIN { for (i = 0; i < 300; i++) { re = "^x" (i % 100) "$"; if (("x" (i % 100)) ~ re) n++ } print n }'
awk 'BEGIN { s = "abcabc"; for (i = 0; i < 100; i++) { t = s; n += gsub(substr("abc", i % 3 + 1, 1), "", t) } print n }'

echo "=== lvalues evaluated once"
awk 'BEGIN { i = 1; a[i++]++; print i, a[1], (2 in a) }'
awk 'BEGIN { i = 1; a[i++] += 5; print i, a[1] }'
echo 'x y z' | awk '{ i = 1; $(i++)++; print i, $0 }'
awk 'BEGIN { i = 0; ++a[++i]; print i, a[1] }'

echo "=== fields are split lazily but with the FS of their record"
printf 'a:b c\nd:e f\n' | awk '{ FS = ":"; print $1 }'
printf 'a b\n' | awk '{ $0 = "x:y"; FS = ":"; print $1 }'
printf 'a b c\n' | awk '{ NF = 2; print $0; print NF }'
printf 'a b c\n' | awk '{ $5 = "e"; print NF; print }'
printf '1 2 3\n' | awk '{ print; $2 = ""; print; print NF }'
printf 'p q\n' | awk '{ x = $0; $1 = "r"; print x, $0 }'
printf 'u v\n' | awk '{ getline line < "/dev/null"; print $2 }'

echo "=== block reads from files"
awk 'BEGIN { s = "x"; for (i = 0; i < 17; i++) s = s s; print s; print "tail" }' > $tmp/long.txt
awk '{ print length($0) }' $tmp/long.txt
printf 'no newline at end' > $tmp/nonl.txt
awk '{ print NR ": " $0 }' $tmp/nonl.txt
printf 'r1;r2;r3' > $tmp/semi.txt
awk 'BEGIN { RS = ";" } { print NR, $0 }' $tmp/semi.txt
printf 'a1b22c333d' > $tmp/digits.txt
awk 'BEGIN { RS = "[0-9]+" } { print NR, $0 }' $tmp/digits.txt
printf 'p1\n\n\np2 line\nmore\n\n' > $tmp/para.txt
awk 'BEGIN { RS = "" } { print NR ": " $1 "|" NF }' $tmp/para.txt
awk 'NR == 1 { while ((getline l < FILENAME) > 0) n++; print n }' $tmp/para.txt
cat $tmp/semi.txt | awk 'BEGIN { RS = ";" } END { print NR }'

echo "=== numbers as strings"
awk 'BEGIN { print 2^53, 2^53 + 2, -0, 1e15, 123456789012345 }'
awk 'BEGIN { a[1] = "one"; a[2.0] = "two"; print a["1"], a["2"] }'

rm -rf $tmp
