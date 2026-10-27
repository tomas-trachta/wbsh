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

echo "=== 10. quoted and escaped pattern characters match literally"
p='a*'
for pat_kind in escaped single double var-quoted var-unquoted; do
    printf "  %-12s ab -> " "$pat_kind"
    case $pat_kind in
        escaped)      case ab in a\*) echo literal;; *) echo no;; esac;;
        single)       case ab in 'a*') echo literal;; *) echo no;; esac;;
        double)       case ab in "a*") echo literal;; *) echo no;; esac;;
        var-quoted)   case ab in "$p") echo literal;; *) echo no;; esac;;
        var-unquoted) case ab in $p) echo glob;; *) echo no;; esac;;
    esac
done
case 'a\b' in 'a\b') echo "  backslash literal";; *) echo "  backslash broken";; esac
case '[x' in '['*) echo "  bracket literal";; *) echo "  bracket broken";; esac
case b in [a"-"c]) echo "  dash in class is a range";; *) echo "  dash in class literal";; esac
case ab in "a"*) echo "  quoted prefix + glob";; esac

echo "=== 11. [[ == ]] honours quoting per segment"
[[ ab == "$p" ]] && echo "  quoted var globbed" || echo "  quoted var literal"
[[ ab == a"*" ]] && echo "  mixed globbed" || echo "  mixed literal"
[[ ab == a\* ]] && echo "  escaped globbed" || echo "  escaped literal"
[[ ab == $p ]] && echo "  unquoted var glob" || echo "  unquoted var literal"

echo "=== 12. quoted patterns in \${var#pat} family"
v='*abc*'
q='*'
echo "  ${v#"*"} ${v%"*"} ${v/"*"/X} ${v//\*/Y} ${v#"$q"} ${v#$q}"
w=abc
echo "  ${w/b/"x"} ${w/b/'$x'} ${w^"a"}"

echo "=== done"
