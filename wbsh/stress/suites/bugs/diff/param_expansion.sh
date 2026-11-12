#!/bin/bash
# Parameter expansion forms.
s='/usr/local/lib/libfoo.so.1.2'
echo "${s##*/} ${s%/*} ${s#*/} ${s%%.*} ${s/lib/LIB} ${s//lib/LIB} ${s/#\/usr/U} ${s/%2/TWO}"
echo "${#s} ${s:5} ${s:5:5} ${s: -3} ${s: -3:2} ${s:(-5)}"
u='Hello World'
echo "${u^^} ${u,,} ${u^} ${u,} ${u~~} ${u^^[lo]}"
unset n; e=
echo "[${n-def}] [${n:-def}] [${e-def}] [${e:-def}] [${n+alt}] [${e+alt}] [${e:+alt}]"
echo "[${n:=set}] [$n]"
a=(alpha beta gamma delta)
echo "${a[@]:1:2} ${#a[@]} ${#a[1]} ${a[-1]} ${!a[@]} ${a[@]/a/A} ${a[@]#?}"
declare -A m=([one]=1 [two]=2)
echo "${#m[@]} ${m[one]} ${m[nope]:-none}"
ref=u; echo "${!ref}"
pre_a=1 pre_b=2; echo "${!pre_@}"
printf -v p '%s' "${s@Q}"; echo "$p"
x='a*b'; echo "${x//\*/+}" "${x//[*]/=}"
n=5; echo "${n@A}" 2>/dev/null || echo noA
