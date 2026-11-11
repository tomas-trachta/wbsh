#!/bin/bash
# Big and deep inputs: wbsh must give an error or a result, never crash or hang.
# A crashed child shows as an exit status above 128 with no wbsh message.
# @stress timeout=300
. "$STRESS_ROOT/lib/check.sh"

# survives CMD -- false when the child wbsh died instead of reporting an error.
survives() {
    wbsh_run "$1"
    [ "$RUN_RC" -ge 128 ] && [ -z "$RUN_ERR" ] && return 1
    case "$RUN_ERR" in
        *[Ss]egmentation*|*[Aa]ccess*violation*|*abort*|*[Ss]tack*overflow*) return 1 ;;
    esac
    return 0
}

severity high
check "deep function recursion (10000) does not crash" survives 'f() { (( $1 > 0 )) && f $(( $1 - 1 )); }; f 10000; echo $?'
check "unbounded recursion stops with an error" survives 'f() { f; }; f'
check "5000 nested parentheses in arithmetic" survives "echo \$(( $(printf '(%.0s' {1..5000})1$(printf ')%.0s' {1..5000}) ))"
check "300 nested command substitutions" survives "echo $(printf '$(echo %.0s' {1..300})x$(printf ')%.0s' {1..300})"
check "3000 nested brace groups" survives "$(printf '{ %.0s' {1..3000})echo x$(printf '; }%.0s' {1..3000})"
check "2000 nested if statements" survives "$(printf 'if true; then %.0s' {1..2000})echo x$(printf '; fi%.0s' {1..2000})"
check "20000-stage && chain" survives "$(printf 'true && %.0s' {1..20000})echo x"

severity medium
wbsh_run 'x=$(head -c 20000000 /dev/zero | tr "\0" a); echo ${#x}'
expect_eq "20 MB command substitution" "$RUN_OUT" 20000000
wbsh_run 'set -- $(seq 1 200000); echo $#'
expect_eq "200000 positional parameters" "$RUN_OUT" 200000
wbsh_run 'printf -v x "%*s" 5000000 ""; y=${x// /ab}; echo ${#y}'
expect_eq "10 MB string replacement" "$RUN_OUT" 10000000
wbsh_run 'a=(); for ((i=0;i<100000;i++)); do a[i*1000]=$i; done; echo ${#a[@]} ${a[99999000]}'
expect_eq "sparse array with large indexes" "$RUN_OUT" "100000 99999"
wbsh_run "$(printf 'echo %.0s' {1..1})$(printf 'a%.0s' {1..100000}) | wc -c"
expect_eq "100000-byte word" "$RUN_OUT" 100001
check "huge allocation request fails cleanly" survives 'printf -v x "%*s" 3000000000 "" 2>/dev/null; echo done'
check "huge brace expansion fails cleanly or completes" survives 'x=( {1..3000000} ); echo ${#x[@]}'
wbsh_run 'echo $(( 2**63 )) $(( -2**63 / -1 )) $(( 9223372036854775807 + 1 ))' 
check "arithmetic overflow does not crash" [ "$RUN_RC" -lt 128 ]
wbsh_run 'echo $(( 1 / 0 ))'
check "division by zero is an error, not a crash" [ "$RUN_RC" -ge 1 -a "$RUN_RC" -lt 128 ]
finish
