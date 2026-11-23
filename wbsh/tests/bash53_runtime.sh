#!/bin/bash
# Runtime semantics as GNU bash 5.3 has them: traps and debugging,
# function export, fd-variable redirections, extglob dot files and xtrace;
# each expected line was checked against bash 5.3.

W=$BASH
T=_run_tmp/bash53_runtime
rm -rf "$T"
mkdir -p "$T"

echo "=== extdebug: a failing DEBUG trap skips the command"
cat > "$T/skip.sh" <<'EOF'
shopt -s extdebug
skip=0
t() { if [[ $skip == 1 ]]; then skip=0; return 1; fi; }
trap t DEBUG
x=1
skip=1
x=2
echo "x is $x"
EOF
"$W" "$T/skip.sh" 2>&1

echo "=== FUNCNAME outside functions"
"$W" -c 'echo "${#FUNCNAME[@]}"; f() { echo "${FUNCNAME[*]}"; }; f; echo "[${FUNCNAME[*]}]"; FUNCNAME=42; echo "$? [$FUNCNAME]"'
printf 'echo "sourced [${FUNCNAME[*]}]"\n' > "$T/fn.sub"
printf '. ./%s\ng() { . ./%s; }\ng\n' fn.sub fn.sub > "$T/fn.sh"
(cd "$T" && "$W" fn.sh)

echo "=== export -f"
foo() { echo "exported foo $1"; }
foo-a() { echo "exported foo-a"; }
export -f foo foo-a
"$W" -c 'foo 1; foo-a; declare -F'
export -fn foo
"$W" -c 'foo 2' 2>&1 | sed 's|^.*line 1: ||'
function a=b { :; }
export -f a=b 2>&1; echo "status $?"
declare -fx bar 2>&1; echo "status $?"
unset -f foo foo-a a=b
env 'BASH_FUNC_x%%=() { echo bad; } ; echo injected' "$W" -c 'x; echo "after $?"' 2>&1 |
	sed 's|^.*line 1: ||; s|^.*: warning|warning|; s|^.*: error importing|error importing|'
env 'BASH_FUNC_y%%=() { echo imported y; }' "$W" -c 'y; declare -F y; env | grep -c BASH_FUNC_y'

echo "=== fd variables"
cat > "$T/vredir.sh" <<'EOF'
readonly v=42
exec {v}>/dev/null
echo "status $?"
exec {w}>&1
exec {x}>&$w-
echo "to x" >&$x
y=nope
exec {y}>&-
echo "close status $?"
echo "to 42" >&$v
f() { exec {z}<&-; exec 3<&- >&-; }
type f
shopt -s varredir_close
: {u}>&1
echo "u still open" >&$u
EOF
(cd "$T" && "$W" vredir.sh 2>&1)

echo "=== extglob and dot files"
shopt -s extglob
mkdir "$T/dots"
(
	cd "$T/dots" && touch .foo bar .a
	echo @(.foo) "|" ?(bar).foo "|" !(bar).foo "|" !(.foo) "|" @(.?)
	shopt -u globskipdots
	echo .* "|" @(.?)
	shopt -s dotglob
	echo * "|" @(*|.*)
	echo !([*)* "|" +(a|b[)*
	case '*(a|b[)' in *(a|b[)) echo "literal group matched";; esac
	touch ';' '++'
	shopt -u dotglob
	GLOBIGNORE='+([^[:alnum:]]):@([-.,:; _]):[![:alnum:]]'
	echo *
)

echo "=== xtrace prefix"
cat > "$T/xtrace.sh" <<'EOF'
PS4='+[$LINENO] '
set -x
echo a
eval 'echo b'
x=$(echo c)
PS4=
echo d
set +x
EOF
"$W" "$T/xtrace.sh" 2>&1
"$W" -c 'echo "[$PS4]"'

echo "=== return in a trap action"
cat > "$T/trapret.sh" <<'EOF'
setexit() { return "$1"; }
trap 'setexit 123; return' USR1
f() { kill -USR1 $$; }
f; echo "f $?"
handler() { setexit 111; return; }
trap 'handler; stat=$?; return' USR1
g() { kill -USR1 $$; return 222; }
g; echo "g $? stat $stat"
EOF
"$W" "$T/trapret.sh" 2>&1

echo "=== declare -ft and the RETURN trap after source"
cat > "$T/ftrace.sh" <<'EOF'
trap 'echo "debug ${FUNCNAME:-main} $LINENO"' DEBUG
h()
{
	:
}
declare -ft h
h
declare -F h
trap - DEBUG
trap 'echo "return ${FUNCNAME:-main} $LINENO"' RETURN
echo : > ret.sub
. ./ret.sub
EOF
(cd "$T" && "$W" ftrace.sh 2>&1)

echo "=== \"\$*\" joins with a multibyte IFS character"
LC_ALL=C.UTF-8 "$W" -c 'IFS=$(printf "\303\251"); set a b; printf "%s\n" "$*" | od -An -b'

echo "=== arithmetic: subscripts, constants, the conditional's false branch"
cat > "$T/arith.sh" <<'EOF'
a=(5 6 7)
echo 1 $(( a[" "] )) $(( a[""] )) $(( a["1"] )) $(( a[ 1 ] ))
(( x = a[\"2\"] )); echo 2 $x
x=$(( 1 + \"2\" )); echo 3 $x
let 'z=a[""]+1'; echo 4 $z
let 'z=a["2"]+1'; echo 5 $z
echo 6 $(( a[] ))
x=1; echo $(( 1 ? 20 : x+=2 )); echo "x=$x"
echo $(( 1 ? x=5 : 3 )) $x $(( 0 ? 1 : 0 ? 2 : 3 ))
echo $(( 2# )); echo $(( 0#4 )); echo $(( 2#110#11 )); echo $(( 3425#56 )); echo $(( 2#44 ))
echo $(( 0x1f )) $(( 010 )) $(( 64#@_ )) $(( 0 ))
echo $(( 08 )); echo $(( 1a ))
P=abcdefg; echo ${P:${OFFSET:-0}}; OFFSET=4; echo ${P:${OFFSET:-0}} ${P:${OFFSET:-0}:2}
i=2; a=(0 0 0)
let 'a[$i]=5'; echo "let 1 ${a[*]}"
let 'a[$(echo 1)]=7'; echo "let 2 ${a[*]}"
iv=; jv=3; let 'jv += $iv'; echo "let 3 $?"
(( jv += \$iv )); echo "dparen $?"
[[ 'a[$i]' -eq 5 ]]; echo "cond 1 $?"
[[ '$i' -eq 2 ]]; echo "cond 2 $?"
set -u
( echo $(( u[0] > 4 )); echo after )
b=(1); ( echo $(( b[3] + 1 )); echo "after b" )
declare -A h; ( echo $(( h[k] + 1 )); echo "after h" )
EOF
"$W" "$T/arith.sh" 2>&1

echo "=== globstar: runs of **, duplicates after a pattern, links"
mkdir -p "$T/gs/a/a/a" "$T/gs/b/a"
touch "$T/gs/a/f" "$T/gs/b/ab"
ln -s a "$T/gs/c"
(cd "$T/gs" && "$W" -c 'shopt -s globstar
echo 1: **; echo 2: **/**; echo 3: a/**; echo 4: a/**/**; echo 5: a/**/; echo 6: **/
echo 7: **/a/**; echo 8: **/a/**/**; echo 9: **/*b; echo 10: **/a')

echo "=== functions: FUNCNEST, odd names, printing, export listing"
cat > "$T/funcs.sh" <<'EOF'
FUNCNEST=5
deep() { (( n++ )); deep; }
n=0; deep; echo "status $? n=$n"
FUNCNEST=0
function a=2 { echo eq; }
function 11111 { echo digits; }
foo-bar() { echo dash; }
declare -f a=2 11111 foo-bar
'a b c' () { echo quoted; }
<(:) () { echo procsub; }
f() { echo a >&2; cat <&3; echo b >&-; echo c >&"$fd"; { echo x; } >&2; }
declare -f f
declare -xF; declare -xf; echo "no exported functions"
EOF
"$W" "$T/funcs.sh" 2>&1 | sed 's/^.*funcs.sh/funcs.sh/'
"$W" -o posix -c 'f-1() { echo dash; }; f-1; function sys$x { :; }; echo not reached' bash 2>&1
echo "posix status $?"

echo "=== RANDOM follows bash's generator once seeded"
"$W" -c 'RANDOM=42; echo $RANDOM $RANDOM $RANDOM $RANDOM; RANDOM=0; echo $RANDOM; RANDOM=42; echo $RANDOM ${ echo $RANDOM; }'

echo "=== a file still open through a redirection can be removed"
(cd "$T" && "$W" -c 'echo data > f; exec 3<f; rm f && echo removed; [ -e f ] || echo gone
read -u 3 line; echo "read $line"; exec 3<&-
exec 4>g; rm g; echo more >&4; exec 4>&-; [ -e g ] || echo "g gone"
mkdir d; echo x > d; echo "status $?"' 2>&1 | sed 's/^.*: line/line/')

rm -rf "$T"
