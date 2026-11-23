#!/bin/bash
# Parsing, quoting and expansion as GNU bash 5.3 has them; each expected
# line was checked against bash 5.3.

W=$BASH
T=_run_tmp/bash53_syntax
rm -rf "$T"
mkdir -p "$T"

run() {
	"$W" "$T/$1" 2>&1 | sed "s|^.*$1|$1|"
}

echo "=== loop headers"
"$W" -c 'for invalid-name in a b; do echo body; done; echo "status $?"' bash 2>&1
"$W" -o posix -c 'for in-valid in a; do :; done; echo "not reached"' bash 2>&1
echo "posix status $?"
"$W" -c 'set --; select i; do echo body; done; echo "empty select $?"' 2>&1
"$W" -c 'for (( i = 0; i < 2; i++ )) { echo "arith $i"; }'
"$W" -c 'for i in a b; { echo "word $i"; }'
"$W" -c 'for (( i=0; "i < 3" )) do :; done' bash 2>&1
"$W" -c 'for (( i=0; i < 3; i++; 7 )) do :; done' bash 2>&1
"$W" -c 'for (( $(case x in x) esac);; )); do echo subst; break; done'
"$W" -c 'for (( ${ case x in x) esac; };; )); do echo funsub; break; done'
"$W" -c 'f() { for (( ; i < 3; i++ )) do :; done; for ((;;)) do break; done; }; declare -f f'

echo "=== pipeline prefixes"
"$W" -c '! time echo a; echo "status $?"' 2>/dev/null
"$W" -c 'time time echo b; echo "status $?"' 2>/dev/null
"$W" -c 'time -p -- echo c; time -- :; echo "status $?"' 2>/dev/null

echo "=== continuation lines"
cat > "$T/cont.sh" <<'EOF'
printf '<%s> ' "${IFS+foo "b\
ar" baz}"; echo
printf '<%s> ' "${IFS+foo 'b\
ar' baz}"; echo
echo `echo 'foo\
bar'`
echo `echo 'foo\\
bar'`
EOF
run cont.sh

echo "=== alias line numbers"
cat > "$T/alias.sh" <<'EOF'
shopt -s expand_aliases
alias foo=$'echo foo\n\nquux_not_found\n'
foo
echo "line $LINENO"
EOF
run alias.sh

echo "=== here-documents cut off by the closing parenthesis"
cat > "$T/heredoc.sh" <<'EOF'
foo=$(cat <<EOF
hi
EOF)
echo $foo
bar=`cat <<EOF
quoted
EOF`
echo $bar
echo $(
cat <<\)
paren
))
EOF
run heredoc.sh

echo "=== unterminated substitutions"
printf 'echo 1\necho $(echo hi\n\necho 3\n' > "$T/eof.sh"
run eof.sh
printf 'echo 1\necho "$(echo hi\n\necho 3\n' > "$T/eofdq.sh"
run eofdq.sh
cat > "$T/late.sh" <<'EOF'
a=4
echo "${a+'$('\'}"; echo "same line"
echo "${a+'$(x'}"
echo "next $?"
EOF
run late.sh

echo "=== brace expansion"
echo `echo foo {1,2} bar`
echo {a..A}
echo {-50..-0..5} {00..10..2}
echo a-{b{d,e}}-c a-{bdef-{g,i}-c {"klklkl"}{1,2,3}
echo {{1,2,3}..{7,8,9}} {6..{7,8,9}}
echo {x,y}{1..a}{0,1,2} {1..20..2f} {1..20..2}
echo {a..,/a.cfg} {1..4,8} {abcde.f} X{..a}Z 0{1..}2
echo {} { } {abc} {x\,y,\{abc\},trie}

echo "=== arithmetic errors"
cat > "$T/arith.sh" <<'EOF'
(( j= ))
(( 7++ ))
(( 7++x ))
(( x=7++ ))
echo $(( 4 -- ))
EOF
run arith.sh

echo "=== special parameters"
"$W" -c 'echo "[$!]"; echo "[${!-unset}]"'
"$W" -uc 'echo $!' 2>/dev/null || echo "set -u rejects \$!"
"$W" -c 'set "a b" c d; printf "<%s> " ${1+"$@"}; echo; printf "<%s> " "${1+$@}"; echo'
"$W" -c 'set "a b" c d; printf "<%s> " "${1-"$@"}"; echo; printf "<%s> " ${1-"$@"}; echo'
"$W" -c 'set "a b" c; printf "<%s> " "${1+"$@"}" "${2:+$@}"; echo'
"$W" -c 'printf "<%s> " ${1+"$@"} "${1-"$@"}" x; echo'

echo "=== single quotes in a double-quoted \${name+word} follow the posix option"
cat > "$T/posix_lines.sh" <<'EOF'
echo 1 "${IFS+'}'z}"
set -o posix
echo 2 "${IFS+'}'z}"
set +o posix
echo 3 "${IFS+'}'z}" "${IFS+'bar} baz'}"
eval 'set -o posix'
echo 4 "${IFS+'}'z}" "foo ${IFS+'bar} baz" line $LINENO
EOF
run posix_lines.sh
"$W" -o posix -c 'echo 5 "${IFS+'"'"'}'"'"'z}"'
POSIXLY_CORRECT=1 "$W" -c 'echo 6 "${IFS+'"'"'}'"'"'z}"'

echo "=== syntax errors name the token the grammar could not take"
syntax_cases=(
	'true &&' 'true && ;' 'true || fi' 'echo |' 'echo | ;' 'echo | fi' '! ;' 'time ;'
	'coproc ;' 'coproc' '(echo hi' 'for x in a; echo; done' 'for x in a b fi'
	'f()' 'f() echo hi' 'f() ;' 'echo >' 'echo > ;' 'cat < |' 'echo 2>' 'echo <<'
	'if true; then echo; fi fi' '{ echo; ' '( )'
	'[[ ( a ]]' '[[ a && ]]' '[[ ]]' '[[ a b ]]' '[[ -z ]]' '[[ a == ]]' '[[ ! ]]'
	'[[ ( -t X ) ]' '[[ -n &' '[[ -n XX &' '[[ -n XX & ]' '[[ 4 & ]]' '[[ 4 > & ]]'
	'[[ & ]]' '[[ -Q 7 ]]' '[[ -n < ]]' '[[ -t X ]]'
)
for c in "${syntax_cases[@]}"; do
	echo "-- $c"
	"$W" -c "$c" bash 2>&1
	echo "status $?"
done

echo "=== \${ list; } ends at a } where a command could start"
cat > "$T/funsub.sh" <<'EOF'
f() { echo outside; }
xx=${ f() { echo inside; }; }
f
echo "${ echo a; { echo b; }; }" "x${ echo c;}y"
echo ${ case x in x) echo d;; esac; } ${| REPLY=e; }
z=${ function g { echo g; }; }; g
echo blank --${
}--
eval 'h() { _; } >_[${ $() }] ;{ echo not reached; }'
echo "status $?"
EOF
run funsub.sh
cat > "$T/funsub_errexit.sh" <<'EOF'
set -e
echo "${ false; echo after false; }"
set -o posix
echo "${ false; echo not reached; }"
echo "not reached either"
EOF
run funsub_errexit.sh
echo "status $?"
echo "=== blanks around \$@ in an operator word follow IFS"
"$W" -c 'recho() { printf "<%s>" "$@"; echo; }; set -- X; X=X; IFS=
recho ${0+ "$@" } ${0+ $@ } ${0+ $* } ${0+ "$X" }; recho ${0+ a b }
set -- a b; recho ${0+ "$@" }; recho "${0+ $@ }"; recho "${0+ $* }"
IFS=" "; recho ${0+ "$@" }; set --; recho "${!*}"'

echo "=== an empty quoted string is a field of its own"
cat > "$T/nulls.sh" <<'EOF'
r() { echo "$# $(printf "<%s>" "$@")"; }
f=" val" e=
r "$e"$f; r $e$f; r "$e""$f"; r ""; r "" ""; set --; r "$@"; r x"$@"; r ""x
x=""; r "$x" a''b; r $'' ""a; r "$e" "$e"; IFS=: ; v=":a:"; r ""$v; IFS=$' \t\n'
y=""; r ${y:-""}; r ${y:-"" ""}; x=x; r ${x:+""}; r ${x:+ ""}; r ${x:+"$e" "$e"""}; r ${x:+'' ''}
a=("" "b"); r "${a[@]}" ""
for p in 0 1 2; do
	set --; [ $p -ge 1 ] && set -- ''; [ $p -ge 2 ] && set -- '' ''
	r "$@"''; r ''"$@"''; r ${x+"$@"}; r ${x+''"$@"''}; r ${x+"$@" "$@"}
done
set -- " A " " B "; IFS=; r ${*} "${*}" ${*##}
EOF
run nulls.sh
"$W" -c 'declare -i a; echo ${a:=4+3}; declare -u A; echo ${A:=foo}'

echo "=== here-document delimiters as bash matches them"
printf '%s\n' 'cat << EOF' 'hi' 'EO\' 'F' 'echo after-split' \
	'cat <<EOF' 'next\' 'EOF' 'EOF' \
	"cat <<-'	END'" '	hello' '	END' 'echo after-tab' \
	'cat <<EO`true`F' 'one' 'EO`false`F' 'EO`true`F' 'echo after-backquote' \
	"cat <<\"Q\$x\"" 'q' 'Q$x' "cat <<''" 'empty' '' 'echo done' > "$T/delims.sh"
run delims.sh

echo "=== more than 16 here-documents on a line"
printf 'cat %s\n' "$(printf '<<E %.0s' {1..17})" > "$T/heredocs.sh"
run heredocs.sh
echo "status $?"

echo "=== a syntax error inside \$(...) stops the command around it"
script_cases=(
	': $( for z in 1 2 3; do ); echo no' ': $( for z in 1 2 3; done ); echo no'
	'echo $( if x; then echo foo ); echo no' 'case x in x) done;; esac'
	': $(case x in x) ;; x) done esac)' ': $(case x in esac|in) foo;; esac)'
	': $(case x in in|esac) foo;; esac); echo ok' ': $(case esac in (esac) echo esac;; esac); echo ok'
	'echo `case x in x) done;; esac`; echo backquotes run late'
)
for c in "${script_cases[@]}"; do
	echo "-- $c"
	printf '%s\n' "$c" > "$T/case.sh"
	"$W" "$T/case.sh" 2>&1 | sed "s|^.*case.sh|case.sh|"
	echo "status ${PIPESTATUS[0]}"
done

rm -rf "$T"
