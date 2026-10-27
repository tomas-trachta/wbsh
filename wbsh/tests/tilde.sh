#!/bin/bash
# Probe: tilde expansion in words and assignment values (bash 5.2 semantics).

HOME=/h
PWD=/p
OLDPWD=/o

echo "=== 1. assignment values expand after each unquoted colon"
x=a:~/b; echo "$x"
x=~:~/c:d~:~e:~; echo "$x"
x=a:~+:~-; echo "$x"
x=a:$HOME:~; echo "$x"
x=a::~; echo "$x"
x=~/b:~; echo "$x"
PATH_LIKE=/usr/bin:~/bin; echo "$PATH_LIKE"

echo "=== 2. quoted or partly quoted prefixes stay literal"
x=a:"~"/b; echo "$x"
x=a:\~/b; echo "$x"
x="a:~/b"; echo "$x"
x=a:~"/b"; echo "$x"
x=~\/b; echo "$x"
x=a:~\:b; echo "$x"
x=a:~x; echo "$x"
x=a=~; echo "$x"
x=~$HOME; echo "$x"

echo "=== 3. append, element and keyed array assignments"
x=a; x+=:~; echo "$x"
a=(x:~ ~); echo "${a[@]}"
a[1]=q:~; echo "${a[@]}"
a=([5]=k:~ [6]=x=~); echo "${a[@]}"
declare -A m
m=([k]=~:~); echo "${m[k]}"

echo "=== 4. prefix assignments"
show() { echo "$V"; }
V=a:~ show

echo "=== 5. words spelled like assignments"
echo a:~/b x=a:~/b y=~ a+=~ a[1]=x:~ e==~
echo 1=~ a-b=~ --opt=~ b:=~ "d"=~
export E=a:~/b; echo "$E"
declare D=a:~/b; echo "$D"
f() { local L=a:~/b; echo "$L"; }; f
readonly R=~:~; echo "$R"
for v in a=~ b:~; do echo "$v"; done
case a=/h in a=~) echo "case pattern matched";; esac

echo "=== 6. command words"
echo ~ ~/x ~:x ~/a:~ ~+ ~- \~ "~"
echo ~$HOME

echo "=== 7. the expansion is not split or globbed"
HOME='/s p'; set -- ~; echo "n=$# [$1]"
x=a:~; echo "$x"
HOME='/*'; echo ~ ~/x
HOME=; set -- ~; echo "n=$# [$1]"
HOME=/h
