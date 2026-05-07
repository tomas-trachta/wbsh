#!/bin/bash
# Probe: brace expansion + case statement edge cases.

echo "=== 1. brace expansion: list"
echo {a,b,c}

echo "=== 2. brace: numeric range"
echo {1..5}

echo "=== 3. brace: numeric range with step"
echo {0..10..2}

echo "=== 4. brace: alpha range"
echo {a..e}

echo "=== 5. brace: prefix and suffix"
echo pre-{a,b,c}-post

echo "=== 6. brace: cartesian product (two adjacent groups)"
echo {a,b}{1,2}

echo "=== 7. brace: nested"
echo {a,{b,c}d}

echo "=== 8. case with multiple patterns"
matchit() {
    case "$1" in
        a|b|c) echo "letter abc";;
        [0-9]) echo "digit";;
        [A-Z]*) echo "starts upper";;
        *.txt) echo "txt file";;
        *.*) echo "has extension";;
        '')    echo "empty";;
        *) echo "other [$1]";;
    esac
}
for x in a b c 1 X HELLO foo.txt bar.cpp baz "" 9z; do
    printf "  %-10s -> " "[$x]"
    matchit "$x"
done

echo "=== 9. case with literal star (quoted glob)"
matchstar() {
    case "$1" in
        '*')  echo "literal star";;
        *)    echo "anything else";;
    esac
}
matchstar '*'
matchstar abc

echo "=== done"
