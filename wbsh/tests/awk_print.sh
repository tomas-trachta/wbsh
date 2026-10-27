#!/bin/bash
# awk print / printf: output redirection, parenthesised argument lists.

tmp=_awk_print_tmp
rm -rf $tmp
mkdir -p $tmp

echo "=== > truncates, >> appends"
awk -v d=$tmp 'BEGIN { print "x" > (d "/o.txt"); print "y" > (d "/o.txt") }'
cat $tmp/o.txt
awk -v d=$tmp 'BEGIN { f = d "/o.txt"; print "z" >> f }'
cat $tmp/o.txt
awk 'BEGIN { print "first" > "'$tmp'/p.txt" }'
cat $tmp/p.txt

echo "=== comparisons inside parentheses"
awk 'BEGIN { print (2 > 1), (1 > 2) }'
awk 'BEGIN { print (3 > 2) > "'$tmp'/c.txt" }'
cat $tmp/c.txt
echo '5 3' | awk '{ print ($1 > $2 ? "big" : "small") }'
echo '5 3' | awk '$1 > $2 { print "pattern ok" }'

echo "=== redirect target is a concatenation"
awk -v d=$tmp 'BEGIN { print "cat" > d "/" "joined.txt" }'
cat $tmp/joined.txt

echo "=== parenthesised lists"
awk 'BEGIN { printf("%d-%s\n", 5, "x") }'
awk 'BEGIN { printf("%s\n", "plain") }'
awk 'BEGIN { print("a", "b") }'
awk 'BEGIN { print ("a", "b") > "'$tmp'/l.txt" }'
cat $tmp/l.txt
awk 'BEGIN { print (1)(2) }'
awk 'BEGIN { print (1+2)*3 }'
awk 'BEGIN { print (1,2) in a }'
awk 'BEGIN { a[1,2] = 1; print ((1,2) in a), ((2,1) in a) }'
awk 'BEGIN { a["x"]; print ("x" in a) > "/dev/stdout" }'
echo 'a b' | awk '{ print $1, $2 > "/dev/stdout" }'
awk 'BEGIN { printf "%s %s\n", "no", "parens" }'
awk 'BEGIN { x = sprintf("%03d", 7); print x }'

echo "=== /dev/stdout and /dev/stderr"
awk 'BEGIN { print "to stdout" > "/dev/stdout" }'
awk 'BEGIN { print "to stderr" > "/dev/stderr" }' 2>&1
awk 'BEGIN { printf "err %d\n", 2 > "/dev/stderr" }' 2>/dev/null
echo "(nothing above)"

rm -rf $tmp
