#!/bin/bash
# Probe: extglob patterns ?(...) *(...) +(...) @(...) !(...) (bash 5.2 semantics).

shopt -s extglob

echo "=== 1. parameter expansion"
x=aaabbb
echo "${x##+(a)} ${x#?(a)} ${x%%*(b)} ${x//@(a|b)/-} ${x/!(a)/X}"
y=hello
echo "${y/+(l)/L} ${y//+(l|o)/_}"

echo "=== 2. case patterns"
for f in foo.txt foo.md foo.c; do
    case $f in
        *.@(txt|md)) echo "$f: text" ;;
        *) echo "$f: other" ;;
    esac
done
case foo in !(bar)) echo "!(bar) matches foo" ;; esac
case bar in !(bar)) echo wrong ;; *) echo "!(bar) does not match bar" ;; esac
case "" in ?(x)) echo "?(x) matches nothing" ;; esac
case xx in ?(x)) echo wrong ;; *(x)) echo "*(x) matches xx" ;; esac
case "" in +(x)) echo wrong ;; *) echo "+(x) needs one" ;; esac
case abab in +(ab)) echo "+(ab) repeats" ;; esac
case "a b" in @(a b)) echo "blank inside a group" ;; esac
case x.tar.gz in *.@(tar.@(gz|bz2))) echo "nested groups" ;; esac
case z in
    a) echo a ;;
    !(a)) echo "!( leading a later pattern" ;;
esac

echo "=== 3. quoting keeps groups literal"
case 'a|b' in @('a|b')) echo "quoted bar is literal" ;; esac
case a in @('a|b')) echo wrong ;; *) echo "quoted bar does not split" ;; esac
case '+(a)' in '+(a)') echo "quoted group matches itself" ;; esac
p='@(a|b)'
case b in $p) echo "group from an unquoted variable" ;; esac

echo "=== 4. [[ == ]] always uses extglob"
shopt -u extglob
[[ foo.txt == *.@(txt|md) ]] && echo "[[ == ]] group"
[[ abc == !(*b*) ]] || echo "[[ == ]] negation"
case a in $p) echo wrong ;; *) echo "case without extglob is literal" ;; esac
case '@(a|b)' in $p) echo "and matches itself" ;; esac
shopt -s extglob

echo "=== 5. pathname expansion"
mkdir -p _run_tmp/extglob && cd _run_tmp/extglob || exit 1
touch a.txt b.md c.c .h.txt
echo !(*.c)
echo *.@(txt|md)
echo +([a-c]).*
echo @(nothing|none)
cd ../..
rm -rf _run_tmp/extglob

echo "=== 6. lexing"
if !(false); then echo "command-leading !( is negation of a subshell"; fi
a[2*(3)]=v
echo "arithmetic subscript: ${a[6]}"
