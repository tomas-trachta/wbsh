#!/bin/bash
# What a new wbsh takes from its environment and its current directory.
# The environment crosses trust boundaries (sudo, cron, CI, git hooks), so
# no variable value may turn into code by accident, and no garbage in the
# WBSH_* state variables may crash the shell.
. "$STRESS_ROOT/lib/check.sh"

severity critical
out=$(env 'x=() { :; }; echo SHELLSHOCK-1' "$STRESS_WBSH" -r -c 'x 2>/dev/null; echo alive' 2>&1 < /dev/null)
expect_no_match "a function-like variable value does not run code (Shellshock)" "$out" "*SHELLSHOCK*"
out=$(env 'BASH_FUNC_x%%=() { :; }; echo SHELLSHOCK-2' "$STRESS_WBSH" -r -c 'echo alive' 2>&1 < /dev/null)
expect_no_match "trailing code after an exported bash function does not run" "$out" "*SHELLSHOCK*"

for f in .wbshrc .bashrc .profile .bash_profile .env; do
    printf 'echo SOURCED-%s\n' "$f" > "$f"
done
out=$("$STRESS_WBSH" -r -c 'echo alive' 2>&1 < /dev/null)
expect_eq "no startup file from the current directory is read" "$out" alive
out=$(echo 'echo alive; exit' | timeout 20 "$STRESS_WBSH" -i 2>&1)
expect_no_match "interactive start reads no startup file from the current directory" "$out" "*SOURCED-*"

severity medium
out=$(env IFS=x "$STRESS_WBSH" -r -c 'a=1x2; set -- $a; echo $#' 2>&1 < /dev/null)
expect_eq "IFS is not imported from the environment (bash resets it)" "$out" 1

for v in BASH_ENV ENV; do
    printf 'echo RAN-%s\n' "$v" > "rc_$v.sh"
    out=$(env "$v=./rc_$v.sh" "$STRESS_WBSH" -r -c 'echo alive' 2>&1 < /dev/null)
    case "$out" in
        *RAN-*) review "\$$v names a file every non-interactive wbsh sources (bash does this for BASH_ENV); document it" ;;
        *) note "\$$v is not sourced by a non-interactive wbsh" ;;
    esac
done
out=$(env 'WBSH_INIT_COMMAND=echo INIT-RAN' "$STRESS_WBSH" -r -c 'echo alive' 2>&1 < /dev/null)
case "$out" in
    *INIT-RAN*) review "WBSH_INIT_COMMAND runs in non-interactive shells too" ;;
esac

severity high
garbage=('}' '{' 'f() {' $'\x01\x02\x03' '%%%' '=' '()' 'a=b=c' "$(printf 'x%.0s' {1..100000})"
         '9999999999999999999999' '-1' $'\n\n\n' '\' '"' "'")
for var in WBSH_FUNCTIONS WBSH_FUNCTION_FILES WBSH_ALIASES WBSH_ARRAYS WBSH_ATTRIBUTES WBSH_BINDINGS \
           WBSH_COMPLETIONS WBSH_DISABLED_BUILTINS WBSH_DIRSTACK WBSH_TRAPS WBSH_JOBS WBSH_HISTORY \
           WBSH_LOCAL_NAMES WBSH_OPTIONS WBSH_LINENO WBSH_SUBSHELL WBSH_STATUS WBSH_PPID WBSH_SHELL_PID \
           WBSH_UTIL_FILES; do
    bad=""
    for g in "${garbage[@]}"; do
        out=$(env "$var=$g" "$STRESS_WBSH" -r -c 'echo alive' 2>/dev/null < /dev/null)
        rc=$?
        if [ "$rc" -ge 128 ] || [ "$out" != alive ]; then
            bad="rc=$rc value=$(printf '%q' "${g:0:20}")"
            break
        fi
    done
    if [ -z "$bad" ]; then
        ok "garbage in $var does not stop the shell"
    else
        fail "garbage in $var breaks the shell ($bad)"
    fi
done
finish
