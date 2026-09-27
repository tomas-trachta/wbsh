#!/bin/bash
# More expansion edge cases (parameter ops, arithmetic, command sub, etc.).

echo "=== parameter expansion"
V=hello-world
echo "${V}"
echo "${V}-extra"
echo "${V/hello/hi}"
echo "${V//-/_}"
echo "${V#hello-}"
echo "${V%-world}"
echo "${V:0:5}"
echo "${V:6}"
echo "${#V}"

echo "=== default / alt"
unset M
echo "${M:-default}"
echo "${M:=assigned}"
echo "M after :=:  $M"
unset M
echo "${M:+only-if-set}"
N=set
echo "${N:+only-if-set}"
echo "${N:-fallback}"

echo "=== array-ish (indexed)"
arr=(one two three four)
echo "${arr[0]}"
echo "${arr[2]}"
echo "${arr[@]}"
echo "${#arr[@]}"

echo "=== command substitution"
x=$(echo nested)
echo "x=$x"
echo "today: $(echo today)"
y=`echo backtick`
echo "y=$y"

echo "=== arithmetic"
echo $((1 + 2 * 3))
echo $(( (1 + 2) * 3 ))
echo $((10 / 3))
echo $((10 % 3))
echo $((2 ** 10))
echo $((1 << 4))
echo $((255 & 15))
echo $((1 | 2 | 4))
echo $((5 > 3 ? 100 : 200))
echo $((! 0))
echo $((! 1))
n=10
echo $((n++))
echo "n after post-inc: $n"
echo $((++n))
echo "n after pre-inc:  $n"

echo "=== brace expansion"
echo {a,b,c}
echo {1..5}
echo {01..03}
echo {a..c}
echo pre{X,Y}post
echo {1..3}{a,b}

echo "=== globbing"
mkdir -p _g
touch _g/a.txt _g/b.txt _g/c.log
echo _g/*.txt
echo _g/*
rm -rf _g

echo "=== quoting"
echo "double $V" 'single $V'
echo "a\"b"
echo "\$literal" '\$literal'

echo "=== done"
