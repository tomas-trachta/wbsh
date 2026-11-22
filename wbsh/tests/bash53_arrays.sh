#!/bin/bash
# Arrays, subscripts and arithmetic as GNU bash 5.3 has them; each
# expected line was checked against bash 5.3.

W=$BASH
T=_run_tmp/bash53_arrays
rm -rf "$T"
mkdir -p "$T"

run() {
	printf '%s\n' "$1" > "$T/case.sh"
	"$W" "$T/case.sh" 2>&1 | sed 's|^.*case\.sh: |case.sh: |'
}

echo "=== text that came from an expansion is never run as a command"
cat > "$T/inject.sh" <<'EOF'
s='$(echo INJECTED >&2; echo 0)'
declare -a a
(( a[$s]++ ))
a[$s]=hi
echo "${#a[$s]}"
echo "${a[$s]}"
declare -i n; n=$s
x='$(echo INJECTED >&2; echo 2)'; echo $(( x ))
unset a["$s"]
shopt -s array_expand_once
printf -v a["$s"] %s hi
read a["$s"] <<<hi
let a["$s"]+=1
test -v a["$s"]
declare -p a
EOF
"$W" "$T/inject.sh" 2>&1 | sed 's|^.*inject\.sh: |inject.sh: |'

echo "=== subscripts: quotes, negative indices, empty keys"
run 'a=(zero one two); echo ${a["1"]} ${a[" 2 "]}; echo ${a[-5]}; echo "after"
c=(); echo "${c[-1]}|"; echo ${#a[-10]}; echo not reached'
run 'b[]=x; echo not reached'
run 'd=([]=abc [1]=one); declare -p d; e=([1]=x [-5]=y); declare -p e'
run 'd[7]=(x y); echo not reached'
run 'f=(1); f+=( [*]=2 ); echo not reached'
run 'a=(1 2); x='"'"'a[$i]'"'"'; i=1; echo $((x)); z='"'"'a[1+]'"'"'; (( z ))'

echo "=== compound assignments"
run 'b=( 1 [1+]=2 3 ); declare -p b'
run 'typeset -i v; v=(4*3)/2; echo $v; let w=(4*3)/2; echo $w'
run 'eval q=(1 "2 3"); declare -p q'
run 'declare -A A=(x x); f() { declare -g A=([1]=1); }; f; declare -p A
g() { declare -ga A=([1]=1); }; g; declare -p A'
run 'declare -A h=([k]=v); h=([j]=w); declare -p h; h=x; declare -p h'
run 'declare -r c[10]; declare -p c; declare +a c'

echo "=== [[ -v ]] and test -v on arrays"
run 'a[1]=set; [[ -v a ]]; echo $?; [[ -v a[1] ]]; echo $?; [[ -v a[@] ]]; echo $?; test -v "a[0+1]"; echo $?'

echo "=== declare listings"
run 'declare -r -a z=(1); x=1; readonly x; readonly -a; set -o posix; readonly -a' | grep -v BASH_VERSINFO
run 'f() { :; }; readonly -f f; declare -Fr; f() { echo again; }; unset -f f; echo "rc=$?"'

echo "=== the star list"
run 'IFS=; A=("" ""); set -- "" ""; echo "<${A[*]:-X}>" "<${*:-X}>"'
run 'set -- a "b c" d; IFS=; printf "<%s>" ${*}x; echo; unset IFS; printf "<%s>" ${*}${IFS=}${*}${IFS:=-}"${*}"; echo'

rm -rf "$T"
