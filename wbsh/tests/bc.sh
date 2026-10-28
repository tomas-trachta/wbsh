#!/bin/bash
# bc: arbitrary precision, scale rules, bases, the language and the -l library.

echo "=== precision"
echo '2^100' | bc
echo '2^256' | bc
echo 'scale=30; 1/3' | bc
echo 'scale=100; sqrt(2)' | bc
echo '123456789012345678901234567890 / 987654321; 123456789012345678901234567890 % 987654321' | bc
echo '10^80' | BC_LINE_LENGTH=20 bc
echo '10^80' | BC_LINE_LENGTH=0 bc

echo "=== scale rules"
echo 'scale=5; 1.23456789 * 1; 1.123 * 1.123; 1.5 * 1.5' | bc
echo '-7 % 3; 7 % -3; scale=2; 7 % 3; -1/3; .5; -0.5; 0.000' | bc
echo 'scale=1; 7.5 % 2; 2^-1; scale=0; 2^-1; 5/2' | bc
echo 'length(123.450); length(0.001); length(0); scale(1.2345)' | bc
echo 'scale=5; sqrt(1); sqrt(0.0004); sqrt(16)' | bc

echo "=== bases"
echo 'obase=16; 255; -255.5' | bc
echo 'obase=2; 10; 0.5' | bc
echo 'obase=100; 12345.678' | bc
echo 'ibase=16; FF; A; 1A.8; obase=10; ibase=A; 10' | bc
echo 'A; 1A' | bc

echo "=== language"
echo 'define f(x) { return x*2 }; f(4)' | bc
bc <<'EOF'
/* recursion and auto variables */
define fact(n) {
    if (n <= 1) return (1);
    return (n * fact(n - 1));
}
fact(30)
define fib(n) {
    auto a, b, t, i
    a = 0; b = 1
    for (i = 0; i < n; i++) {
        t = a + b; a = b; b = t
    }
    return a
}
fib(100)
define sum(x[], n) {
    auto i, t
    for (i = 0; i < n; i++) t += x[i]
    return t
}
for (i = 0; i < 5; i++) v[i] = i * i
sum(v[], 5)
define byref(*a[]) { a[0] = 42; }
define byval(a[]) { a[0] = 7; }
byref(w[]); w[0]
byval(w[]); w[0]
define void hello(n) {
    print "hello ", n, "\n"
}
hello(3)
i = 0
while (1) {
    i = i + 1
    if (i == 3) continue
    if (i > 5) break
    print i, " "
}
print "\n"
if (1 > 2) {
    "no
"
} else {
    print "yes\n"
}
define g(x)
{
    if (x > 2) {
        return x
    }
    else {
        return
    }
}
g(5); g(1)
a = 5; a; a++; a; ++a; (b = 3); -2^2; 2^3^2
c = 3 < 4; c
!0; !5; !1 < 2; 3 == 3 && 2 > 1 || 0
last; .
# comment
1 + \
2
print "a\tb\qc\\d\n"
EOF

echo "=== quit and halt"
printf '1\nquit\n2\n' | bc
printf 'if (0) quit\n5\n' | bc
echo 'x = 2; halt; x' | bc
echo "rc=$?"

echo "=== errors"
echo '1/0; 2' | bc 2>&1
echo 'sqrt(-1)' | bc 2>&1
echo '1 +; 3' | bc 2>&1
echo 'foo(1)' | bc 2>&1
echo 'a[-1] = 2' | bc 2>&1
echo '1 @ 2' | bc 2>&1
echo '2^1.5' | bc 2>&1
echo 'ibase=1; obase=1' | bc 2>&1
bc /nonexistent/file.bc </dev/null 2>&1
echo "rc=$?"
bc -z </dev/null 2>&1 | head -1

echo "=== options"
bc -e '3+4' -e '2*5'
printf '21\n' | bc -e 'read() * 2'

echo "=== math library"
echo '4*a(1); e(1); l(2); s(1); c(1); j(0,1)' | bc -l
echo 'e(-1); l(0.5); a(-2); s(100); e(10); l(1000000)' | bc -l
echo 'scale=60; j(3, 10.5); j(-3, 2)' | bc -l
echo 'scale=2; e(0); c(0); l(1); l(0)' | bc -l
bc -l -e 'scale=50; 4*a(1)'
