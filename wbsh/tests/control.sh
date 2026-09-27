#!/bin/bash
# Smoke test for control flow / scoping edges.

echo "=== if / elif / else"
n=42
if [ $n -gt 100 ]; then
    echo big
elif [ $n -gt 10 ]; then
    echo medium
else
    echo small
fi

if true; then echo true-branch; fi
if false; then echo no; else echo else-branch; fi

echo "=== for in"
for x in alpha beta gamma; do echo $x; done
echo --
for n in $(seq 1 3); do echo "n$n"; done

echo "=== while / until"
i=0
while [ $i -lt 3 ]; do
    echo "while $i"
    i=$((i+1))
done

i=5
until [ $i -le 0 ]; do
    echo "until $i"
    i=$((i-1))
done

echo "=== break / continue"
for i in 1 2 3 4 5; do
    [ $i -eq 3 ] && continue
    [ $i -eq 5 ] && break
    echo "iter $i"
done

echo "=== break N"
for i in 1 2 3; do
    for j in a b c; do
        [ $j = "b" ] && [ $i -eq 2 ] && break 2
        echo "$i$j"
    done
done

echo "=== case (with ;& fallthrough not assumed)"
for v in foo bar baz qux; do
    case $v in
        foo) echo "F" ;;
        bar) echo "B" ;;
        ba*) echo "B*" ;;
        *)   echo "?" ;;
    esac
done

echo "=== nested funcs / recursion"
fact() {
    if [ $1 -le 1 ]; then
        echo 1
    else
        local p=$(fact $(($1 - 1)))
        echo $(($1 * p))
    fi
}
echo "5! = $(fact 5)"
echo "7! = $(fact 7)"

echo "=== exit-status flow"
true && echo and-after-true
false || echo or-after-false
true && false || echo "chain"

echo "=== subshell isolation"
x=outer
(x=inner; echo "in: $x")
echo "out: $x"

echo "=== done"
