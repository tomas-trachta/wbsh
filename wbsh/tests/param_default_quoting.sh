#!/bin/bash
# An unquoted ${u:-word} or ${v:+word} keeps the quoting of its word, as in
# bash: a quoted or escaped `*` is a plain `*` and does not glob, a quoted
# space does not split, and `""` alone still gives one empty field. What a
# variable puts in the word is not quoted. ${u:=word} assigns the word
# after quote removal; the result is the new value, which globs.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
touch a1 a2 ab

echo 1 ${u:-\*}
echo 2 ${u:-a\*}
echo 3 ${u:-a*}
echo 4 "${u:-a\*}"
echo 5 ${u:-\a}
echo 6 ${u:-a\?}
echo 7 ${u:-'a*'}
echo 8 ${u:-"a*"}
echo 9 ${u:-a"*"}
echo 10 ${u:-\[a]1}
echo 11 ${u:-'\*'}
x='a\*'
echo 12 $x ${u:-$x}
pat='a*'
echo 13 ${u:-$pat} ${u:-"$pat"}

case 'a*' in ${u:-a\*}) echo 14 match ;; *) echo 14 no ;; esac
case 'ab' in ${u:-a\*}) echo 15 match ;; *) echo 15 no ;; esac

echo 16 ${u:=\*}
echo "16b $u"
unset u

echo 17 ${v:+\*} ${v:+'a*'}
v=1
echo 18 ${v:+\*} ${v:+'a*'} ${v:+a*}

w='p q'
set -- ${u:-$w}; echo "19 $#"
set -- ${u:-"$w"}; echo "20 $#"
set -- ${u:-a\ b}; echo "21 $#"
set -- ${u:-x y}; echo "22 $#"
set -- ${u:-""}; echo "23 $#"
set -- ${u:-"$e"}; echo "24 $#"
set -- ${v:+""}; echo "25 $#"
set -- ${u:-$w"c d"}; echo "26 $# [$2]"

echo 27 ${u:-${z:-\*}}
echo 28 ${u:-"$w"*}
r=u
echo 29 ${!r:-\*}

# The list forms keep the quoting of their word too.
e=()
set -- ${e[@]:-\*}; echo "30 $# [$*]"
set -- ${e[@]:-"x y"}; echo "31 $# [$*]"
set -- ${e[@]:-x y}; echo "32 $# [$*]"
set -- ${e[@]:-""}; echo "33 $#"
f=(1)
set -- ${f[@]:+"x y"}; echo "34 $# [$*]"
set --
set -- ${@:-"x y"}; echo "35 $# [$*]"
set -- ${e[*]:-a*}; echo "36 $# [$*]"
g=(p "q r")
set -- ${g[@]:-d}; echo "37 $#"
set -- "${g[@]:-d}"; echo "38 $#"

cd / && rm -rf "$D"
