#!/bin/bash
# Builtin options, messages and statuses as GNU bash 5.3 has them; each
# expected line was checked against bash 5.3.

W=$BASH
T=_run_tmp/bash53_builtins
rm -rf "$T"
mkdir -p "$T"

echo "=== exit, return, shift with bad operands"
cat > "$T/numeric.sh" <<'EOF'
exit abc; echo "after exit $?"
f() { return abc; echo "in f $?"; }; f; echo "after f $?"
set -- 1 2; shift abc; echo "after shift $? $#"
exit 1 2; echo "not reached"
echo "next line $?"
return abc; echo "toplevel $?"
for i in 1; do break -- 1; done; echo "break -- $?"
EOF
"$W" "$T/numeric.sh" 2>&1 | sed 's|^.*numeric.sh|numeric.sh|'
r=$("$W" -o posix -c 'for f in _; do exit abcde; done; echo "posix after exit"' 2>&1)
echo "posix exit status $?: ${r#*line 1: }"
r=$("$W" -o posix -c 'f() { return abcde; }; f; echo "posix after return"' 2>&1)
echo "posix return status $?: ${r#*line 1: }"

echo "=== shopt defaults and names"
"$W" -c 'shopt globstar; shopt -p xyz1; shopt -o -p xyz1' 2>&1 | sed 's|^.*line 1: ||'
"$W" -c 'shopt -s restricted_shell; shopt restricted_shell; shopt syslog_history' 2>&1 |
	sed 's|^.*line 1: ||'

echo "=== readonly arrays and compound assignments"
cat > "$T/readonly.sh" <<'EOF'
a=(outside)
f2() { readonly -a a=(2) ; }
f4() { readonly -a 'a=(4)' ; }
f1() { readonly a=(1) ; }
f1
f2
f4
c=(outside)
f3() { readonly 'c=(3)' ; }
f3
declare -p a c
r=(1); readonly r
g() { declare -a r=(6); echo "g $?"; }; g
h() { echo hi; readonly -a r=(6); echo "not reached"; }; h
echo "after h $?"
declare -a r; echo "declare -a $?"
EOF
"$W" "$T/readonly.sh" 2>&1 | sed 's|^.*readonly.sh|readonly.sh|'

echo "=== read"
echo " a  b\ " | ( read x y ; echo -"$x"-"$y"- )
echo " a  b\ " | ( read x ; echo -"$x"- )
(
	readonly b
	read a b c <<< "a b c"
	echo "a=$a b=$b c=$c stat=$?"
) 2>&1 | sed 's|^.*line [0-9]*: ||'
printf '  line\tb \t\r\f\v\n' | { IFS=$'\t\r\f\v'; read v1 v2; printf '<%s><%s>\n' "$v1" "$v2"; }

echo "=== printf"
printf -x 2>&1; echo "status $?"
printf -v bad-name x 2>&1; echo "status $?"
printf '%s%n|%s\n' abcd n1 xy; echo "n1=$n1"
printf '%.1Q|%#q|%#q\n' '**' plain 'a;b'
printf '<%q>\n' $'B\315' 2>&1
printf -- '--%b--\n' '\t\101' '\1017' '\x' 2>&1
printf '%d\n' 9223372036854775825 '' 2>&1; echo "status $?"
printf '%(abde)Z|\n' -1 2>&1
printf '[%-12.3(%Y (x) %m)T]\n' 0
x=$'B\315'; declare -p x

echo "=== test"
test 4+3 -eq 7 2>&1; echo "status $?"
[ \( 1 = 2 ] 2>&1; echo "status $?"
test 4 -eq 4 -a 2 -ne 5 -a 4 -ne 2>&1; echo "status $?"
test -n xx -a -z "" -a -t 0 -t 2>&1; echo "status $?"
test -t a 2>&1; echo "status $?"
test -w /dev/null; echo "-w /dev/null $?"

echo "=== type, alias, exec"
set -o posix; type break; set +o posix
hash -p /tmp/fake-tool faketool
type faketool; type -p faketool; type -t faketool
alias '\$'=xx 2>&1; echo "status $?"
exec -i 2>&1; echo "exec status $?"

echo "=== cd, umask, mapfile, misc"
mkdir -p "$T/cdp/sub"
(CDPATH=.:"$PWD/$T/cdp"; cd sub > /dev/null && echo "via CDPATH: ${PWD##*/}")
(cd one two 2>&1; echo "status $?")
(unset HOME; cd 2>&1; echo "status $?")
(umask 022; umask -S; umask g+w; umask; umask -p -S; umask 09 2>&1; umask u:rwx 2>&1; umask g=p 2>&1; umask)
mapfile -u XX A 2>&1; mapfile '' < /dev/null 2>&1; echo "status $?"
let 2>&1; echo "status $?"
shopt -s -u checkhash 2>&1; echo "status $?"
unset -f -v SHELL 2>&1; echo "status $?"
trap -s 2>&1; echo "status $?"
eval -i 2>&1; echo "status $?"
for z in 1 2 3; do break 0 2>&1; echo "not reached"; done; echo "status $?"
unalias 2>&1; echo "status $?"

echo "=== trap -P and lone operands"
cat > "$T/trap_p.sh" <<'EOF'
trap 'echo exiting' EXIT
trap -P EXIT
trap -p -P; echo "status $?"
trap -P; echo "status $?"
trap ''; echo "status $?"
trap 512; echo "status $?"
trap 0; trap -P EXIT; echo "status $?"
trap 'echo x' INT; trap -P INT EXIT
trap -P bogus; echo "status $?"
trap INT; trap -p INT; echo "reset $?"
set -o posix; trap INT; echo "posix status $?"
EOF
"$W" "$T/trap_p.sh" 2>&1 | sed 's/^.*trap_p.sh/trap_p.sh/'

echo "=== expr follows GNU expr's grammar"
t() { printf '%s => ' "$*"; out=$(expr "$@" 2>/dev/null); echo "[$out] $?"; }
t "4.2MP" : '[^0-9]*[0-9]*.\([0-9]*\)'; t abc : 'a\(b\)'; t abc : ab; t abc : 'x\(y\)'
t 1 + 2; t 2 \* 3 + 1; t 7 / 0; t a + 1; t 0 \| ''; t '' \| 5; t 3 \& 0; t 3 \& 4
t a \< b; t 10 \< 9; t 10 '<' 9a; t length abcd; t substr hello 2 3; t index hello lo
t match abc 'a\(.\)'; t \( 1 + 2 \) \* 3; t \( 1 + 2; t 1 +; t 1 2; t + length; t length
t 00; t -0; t; t -- 5

echo "=== job, kill, unset and history errors"
cat > "$T/errs.sh" <<'EOF'
declare -a AA
unset AA[-2]; echo "s $?"
bg; echo "s $?"
fg; echo "s $?"
kill; echo "s $?"
kill -s; echo "s $?"
kill -S; echo "s $?"
kill -INT ''; echo "s $?"
kill -INT; echo "s $?"
kill -l SIGBAD; echo "s $?"
kill -HUP @12; echo "s $?"
unset -v BASH_LINENO BASH_SOURCE; echo "s $?"
set -o history
HISTFILE=/dev/null
echo a >/dev/null
history abcde; echo "after history: $?"
history 10 42; echo "after history: $?"
EOF
"$W" "$T/errs.sh" 2>&1 | sed 's/^.*errs.sh/errs.sh/'
cat > "$T/errs2.sh" <<'EOF'
set -- a b c
for f in _; do shift 42 abcde; done
echo after shift: $?
f() { for g in _; do return 42 abcde; done; }; f
echo after return: $?
array[1]=one; set -u; ( echo ${#array} ); ( echo ${array} ); ( echo ${array[7]} ); echo end
EOF
"$W" "$T/errs2.sh" 2>&1 | sed 's/^.*errs2.sh/errs2.sh/'

echo "=== character devices are only character devices"
for t in -f -d -s -c -e -r -w; do
	[ $t /dev/null ]; printf '%s:%s ' "$t" "$?"
done
echo

rm -rf "$T"
