#!/bin/bash
# awk: user functions, command pipes, ENVIRON, ARGV/ARGC, RS, OFMT/CONVFMT, -v escapes.

tmp=_awk_posix_tmp
rm -rf $tmp
mkdir -p $tmp
printf 'one\ntwo\nthree\n' > $tmp/lines.txt
printf 'x 1\ny 2\n' > $tmp/pairs.txt

echo "=== user functions"
awk 'function f(x){return x*2} BEGIN{print f(21)}'
awk 'BEGIN { print fact(10) } function fact(n) { return n <= 1 ? 1 : n * fact(n - 1) }'
awk 'function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2) } BEGIN { print fib(20) }'
awk 'function swap(a, i, j,   t) { t = a[i]; a[i] = a[j]; a[j] = t }
BEGIN { v[1] = "a"; v[2] = "b"; swap(v, 1, 2); print v[1], v[2]; print (t == "") }'
awk 'function fill(arr, n,   i) { for (i = 1; i <= n; i++) arr[i] = i * i } BEGIN { fill(sq, 4); print length(sq), sq[3] }'
awk 'function bump(x) { x++; return x } BEGIN { y = 5; print bump(y), y }'
awk 'function noval() { return } BEGIN { x = noval(); print "[" x "]", length(x) }'
awk 'function g(a) { a["k"] = 1 } function h(b) { g(b) } BEGIN { h(z); print ("k" in z) }'
awk 'func twice(s) { return s s }
BEGIN { print twice("ab") }'
awk 'function max(a, b) { return a > b ? a : b } { m = max(m, $1) } END { print m }' <<'EOF'
3
17
9
EOF
awk 'function f(x) { if (x > 2) return "big"; return "small" } BEGIN { print f(1), f(5) }'
awk 'function count(s,   n) { while (s ~ /a/) { sub(/a/, "", s); n++ } return n } BEGIN { print count("banana") }'
awk 'function nxt() { next } NR == 2 { nxt() } { print }' $tmp/lines.txt
awk 'BEGIN { print f(1) }' 2>&1
awk 'function f(a) { return a } function f(b) { return b } BEGIN { }' 2>&1

echo "=== command pipes, system, close"
awk 'BEGIN { "echo hello" | getline d; print "got", d }'
awk 'BEGIN { while (("printf \"a\\nb\\nc\\n\"" | getline line) > 0) n++; print n, line }'
awk 'BEGIN { cmd = "echo first"; cmd | getline; print $0, NR; close(cmd); cmd | getline x; print x }'
awk 'BEGIN { print "b" | "sort"; print "a" | "sort"; print "c" | "sort"; close("sort"); print "after" }'
awk '{ print $2, $1 | "sort -r" } END { print "end-before-flush" }' $tmp/pairs.txt
awk 'BEGIN { r = system("exit 3"); print "status", r }'
awk 'BEGIN { printf "before "; system("echo inside"); print "after" }'
awk 'BEGIN { print "x" | "cat 1>&2"; print close("cat 1>&2") }' 2>&1
awk 'BEGIN { print close("never-opened") }'
awk 'BEGIN { f = "'$tmp'/out.txt"; print "1" > f; close(f); print "2" > f; close(f); while ((getline l < f) > 0) print "read", l }'
awk 'BEGIN { system("x=inner"); "echo $x" | getline v; print "[" v "]" }'
awk 'BEGIN { printf "flushed "; fflush(); system("echo ok") }'

echo "=== ENVIRON"
FOO=bar awk 'BEGIN { print ENVIRON["FOO"] }'
export AWK_TEST_VAR="with space"
awk 'BEGIN { print ENVIRON["AWK_TEST_VAR"] }'
NUM=10 awk 'BEGIN { print (ENVIRON["NUM"] > 9) }'
FOO=prefix printenv FOO
echo "FOO after: [${FOO-unset}]"

echo "=== ARGV and ARGC"
awk 'BEGIN { print ARGC, ARGV[0], ARGV[1], ARGV[2] }' foo bar
awk 'BEGIN { for (i = 0; i < ARGC; i++) printf "%s|", ARGV[i]; print "" }' a "b c"
awk '{ print FILENAME ": " $0 }' $tmp/pairs.txt
awk 'BEGIN { ARGV[1] = "'$tmp'/pairs.txt" } { print }' $tmp/lines.txt
awk 'BEGIN { ARGV[1] = "" } { print "stdin:", $0 }' $tmp/lines.txt <<'EOF'
from stdin
EOF
awk 'BEGIN { ARGC = 2 } { print }' $tmp/pairs.txt $tmp/lines.txt
awk '{ print v, $0 }' v=1 $tmp/pairs.txt v=2 $tmp/pairs.txt
awk '{ print v ":" $0 } END { print "end", v }' v=a $tmp/pairs.txt v=z
echo piped | awk '{ print x, $0 }' x=set
awk '{ print FNR, NR, $0 }' $tmp/pairs.txt $tmp/pairs.txt
awk '{ print }' $tmp/missing.txt $tmp/pairs.txt 2>&1
echo "status $?"

echo "=== RS"
printf 'a\nb\n\nc\n' | awk 'BEGIN{RS=""}{print NR": "$1}'
printf '\n\nname1\nline1\n\n\n\nname2\nline2\n\n' | awk 'BEGIN { RS = "" } { print NR, NF, $1, $NF }'
printf 'a:b\nc:d\n\ne:f\n' | awk 'BEGIN { RS = ""; FS = ":" } { print NF; for (i = 1; i <= NF; i++) printf "[%s]", $i; print "" }'
printf 'a;b;c' | awk 'BEGIN { RS = ";" } { print NR, $0 }'
printf 'one, two,three' | awk 'BEGIN { RS = ", *" } { print NR "<" $0 ">" }'
printf 'x1y22z' | awk -v RS='[0-9]+' '{ print $0 }'
printf 'l1\nl2\n' | awk 'BEGIN { RS = "\n" } END { print NR }'

echo "=== OFMT and CONVFMT"
awk 'BEGIN{OFMT="%.2f"; print 3.14159}'
awk 'BEGIN { OFMT = "%.2f"; x = 3.14159; y = x ""; print x, y, 10 }'
awk 'BEGIN { CONVFMT = "%.3g"; x = 3.14159; y = x ""; print y; a[x] = 1; for (k in a) print k }'
awk 'BEGIN { CONVFMT = "%d"; print (0.5 "") }'
awk 'BEGIN { print 1e6, 1e-5, 123456789012, 0.1 }'

echo "=== -v and operand escapes"
awk -v s='a\tb' 'BEGIN{print s}'
awk -v s='line1\nline2' 'BEGIN { print s }'
awk -v s='q\"x\\y' 'BEGIN { print s }'
awk -v n=010 'BEGIN { print n + 0, (n == 10) }'
awk '{ print s }' 's=x\ty' $tmp/pairs.txt | head -1
awk -F '\t' '{ print $2 }' <<'EOF'
a	b	c
EOF
awk -v 'bad-name=1' 'BEGIN { }' 2>&1
echo "status $?"

echo "=== getline forms"
awk 'NR == 1 { getline; print "after getline:", $0, NR, FNR } END { print NR }' $tmp/lines.txt
awk 'NR == 1 { getline nextline; print $0, "/", nextline, NR } END { print NR }' $tmp/lines.txt
awk 'BEGIN { while ((getline line < "'$tmp'/lines.txt") > 0) n++; print n, NR }'
awk 'BEGIN { getline < "'$tmp'/lines.txt"; print $0, NF, NR }'
awk 'BEGIN { print (getline x < "'$tmp'/nope.txt") }'
awk 'BEGIN { getline; print "begin read:", $0 } { print "main:", $0 }' $tmp/lines.txt
awk 'BEGIN { getline a[1] < "'$tmp'/pairs.txt"; print a[1] }'

echo "=== exit runs END"
awk 'BEGIN { exit 3 } END { print "end ran" }'
echo "status $?"
awk '{ exit } END { print "end after", NR }' $tmp/lines.txt
awk 'END { exit 4; print "not printed" } END { print "nor this" }' < /dev/null
echo "status $?"

echo "=== misc"
echo 'hello world' | awk 'length > 5'
echo 'hello world' | awk '{ print length }'
echo 'foo bar' | awk '{ x = /foo/; y = /baz/; print x, y }'
awk 'BEGIN { printf "%*d|%-*s|%.*f\n", 5, 42, 4, "ab", 2, 3.14159 }'
awk 'BEGIN { print substr("hello", 0, 2), substr("hello", -1), substr("hello", 2) }'
awk 'BEGIN { print index("abc", ""), index("abc", "c") }'
awk 'BEGIN { print -2^2, 2^3^2, 2**3 }'
awk 'BEGIN { s = sprintf("%c%c", 72, "i!"); print s }'
awk 'BEGIN { n = split("", parts); print n, length(parts) }'
awk 'BEGIN { n = split("abc", ch, ""); print n, ch[1], ch[3] }'
awk 'BEGIN { if (!(3 in a)) print "no 3"; if (a[3] == "") print "now", length(a) }'
echo '1 2 3' | awk '{ $5 = "x"; print; print NF }'
echo '1 2 3' | awk '{ NF = 2; print; $0 = "a b c d"; print NF }'
echo 'a b c' | awk '{ $2 = ""; print; print NF }'
printf 'r1\nr2\nr3\nr4\nr5\n' | awk 'NR==2, NR==4'
printf 'start\nx\nend\nstart end\ny\n' | awk '/start/, /end/ { print NR": "$0 }'
awk 'BEGIN { x["a"]; delete x; print length(x) }'
awk 'BEGIN { if (1) ; else print "no"; print "empty if ok" }'

rm -rf $tmp
