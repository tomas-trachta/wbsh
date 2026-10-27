#!/bin/bash
# Probe: ${!name} indirection, ${!prefix*} / ${!prefix@}, and the
# default operators on array elements (bash 5.2 semantics).

echo "=== 1. indirection with operators"
x=y; y=val; echo ${!x:-def} ${!x/v/V}
x=y; y=hello
echo "${!x} ${!x:-d} ${!x-d} ${!x:+alt} ${!x+alt} ${!x#he} ${!x%lo}"
echo "${!x/l/L} ${!x//l/L} ${!x^} ${!x^^} ${!x:1} ${!x:1:2} ${!x:0:1}"
x=nope; echo "[${!x}] ${!x:-def} ${!x-def} [${!x:+alt}]"
x=nope; : ${!x:=assigned}; echo "nope=$nope"
x=y; y=; echo "[${!x:-d}] [${!x-d}]"
unset x; echo "unset ref: [${!x}] [${!x-d}] [${!x:-d}] [${!x+a}]"

echo "=== 2. positional and special targets"
set -- a b c
echo "${!#} ${!1-u}"
n=2; echo "${!n}"
x=1; set -- p q; echo "${!x} ${!x/p/P}"
x='#'; echo "${!x}"
x='@'; set -- 1 2; echo "[${!x:-z}] [${!x/1/X}]"
printf '<%s>' "${!x}"; echo

echo "=== 3. array targets"
x='a[1]'; a=(p q); echo ${!x}
x=a; a=(p q r); echo "${!x} [${!x[1]}]"
x='a[@]'; echo "${!x}"; printf '<%s>' "${!x}"; echo
x='a[*]'; printf '<%s>' "${!x}"; echo
x='a[@]'; a=(p q); echo "[${!x/p/P}] ${!x:1}"
x='a[2]'; a=(p q r); echo "${!x:-d} ${!x/r/R}"
x='a[i]'; i=1; echo "${!x}"
declare -A h
h=([k]=v [j]=w)
x='h[k]'; echo "${!x}"
r=(a h); echo "${!r[0]} [${!r[1]}]"
x=s; s=one; echo "[${!x[1]-d}] [${!x[0]-d}]"
x=y; y='a b'; set -- ${!x}; echo "split: $#"
set -- "${!x}"; echo "quoted: $#"

echo "=== 4. names with a prefix"
myvar1=1; myvar2=2; myvarx=3; other=4
echo "${!myvar*}"
echo "${!myvar@}"
printf '<%s>' "${!myvar@}"; echo
printf '<%s>' "${!myvar*}"; echo
IFS=,; printf '<%s>' "${!myvar*}"; echo; unset IFS
printf '<%s>' ${!myvar@}; echo
myarr=(1); declare -A myassoc; myassoc[k]=v; echo "${!my*}"
echo "[${!nomatch*}] [${!nomatch@}]"
printf '<%s>' "${!nomatch@}"; echo "|"
for v in "${!myvar@}"; do echo "v=$v"; done
echo "${!RAND*} ${!SECOND*} ${!LINEN*}"
pfx=$(echo "${!myvar@}"); echo "in \$(): $pfx"

echo "=== 5. errors"
x='a b'; (echo "[${!x}]"); echo "rc=$?"
x='1a'; (echo "[${!x}]"); echo "rc=$?"
x=; (echo "[${!x}]"); echo "rc=$?"
x='a[1'; (echo "[${!x}]"); echo "rc=$?"
x='$y'; (echo "[${!x}]"); echo "rc=$?"
x='y:-q'; (echo "[${!x}]"); echo "rc=$?"
unset x; (: "${!x:=v}"; echo "x=[$x]"); echo "rc=$?"
x=y; unset y; (echo "[${!x:?}]"); echo "rc=$?"
(set -u; x=unset_target; echo "${!x}"); echo "rc=$?"
(set -u; unset x; echo "${!x}"); echo "rc=$?"
(set -u; x=unset_target; echo "${!x:-ok}")

echo "=== 6. default operators on array elements"
x=a; echo "[${x[1]-d}] [${x[0]-d}] [${x[1]:-d}] [${x[1]+set}]"
a=(p '' q); echo "[${a[1]-d}] [${a[1]:-d}] [${a[5]-d}] [${a[5]+s}] [${a[0]+s}]"
unset h; declare -A h; h[k]=v; echo "[${h[k]-d}] [${h[z]-d}] [${h[z]+s}]"
: "${a[7]:=seven}"; echo "${a[@]}"
: "${h[n]:=new}"; echo "${h[n]}"
i=0; echo "${a[i++]-d} i=$i"
(echo "[${a[9]:?}]"); echo "rc=$?"
