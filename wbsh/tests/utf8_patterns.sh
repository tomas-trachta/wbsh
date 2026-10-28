#!/bin/bash
# Probe: UTF-8 aware string operators and glob patterns, POSIX bracket classes
# (bash 5.2 in a UTF-8 locale).

echo "=== 1. length and substrings count characters"
x=héllo
echo "${#x} ${x:1:1} ${x:1:3} ${x: -3} ${x:0:-2} ${x:2}"
s=日本語テキスト
echo "${#s} ${s:2:2} ${s#日本} ${s%テキスト}"
a=(héllo wörld)
echo "${#a[0]} ${#a[1]} ${a[1]:1:1}"

echo "=== 2. ? and brackets match one character"
case é in ?) echo "? matches é" ;; esac
case é in ??) echo "?? matches é" ;; *) echo "?? does not match é" ;; esac
case é in [é]) echo "[é] matches é" ;; esac
case é in [a-z]) echo "[a-z] matches é" ;; *) echo "[a-z] does not match é" ;; esac
case ž in [ā-ž]) echo "code point range" ;; esac
[[ é == ? ]] && echo "[[ == ? ]] matches é"
y=čřžýá
echo "${y#?} ${y%?} ${y##*ř} ${y%%ž*} ${y/ř/R} ${y//?/.}"

echo "=== 3. POSIX classes"
for c in a é É 5 ' ' _ . f; do
    printf '[%s]' "$c"
    for cls in alpha digit alnum upper lower space blank punct xdigit word print graph; do
        case $c in [[:$cls:]]) printf ' %s' $cls ;; esac
    done
    echo
done
case a in [[:alpha:][:digit:]]) echo "two classes" ;; esac
case 1 in [![:alpha:]]) echo "negated class" ;; esac
case b in [[=b=]]) echo "equivalence class" ;; esac
case - in [[.-.]]) echo "collating symbol" ;; esac
[[ ab == [[:alpha:]]? ]] && echo "class in [[ == ]]"

echo "=== 4. an unterminated [ is literal"
case '[a' in [a) echo "[a matches itself" ;; esac
case '[' in [) echo "[ matches itself" ;; esac
case x in [) echo wrong ;; *) echo "[ does not match x" ;; esac
case ']' in []]) echo "] first is a member" ;; esac
case 'a]' in [a]]) echo "[a]] is a class then ]" ;; esac

echo "=== 5. case conversion"
echo "${y^} ${y^^} ${x^^[lé]}"
z=ČŘŽ
echo "${z,} ${z,,}"
echo "${a[@]^}"

echo "=== 6. \${...} holds no brace expansion"
v=AB
echo ${v,} ${v,,} {1,2}${v,} {h,${v/A/,}}
