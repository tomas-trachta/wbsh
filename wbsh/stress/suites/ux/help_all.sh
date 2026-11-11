#!/bin/bash
# Every builtin and bundled command answers --help quickly, without stdin,
# and has a manual entry. A command that hangs here would hang a user who
# types "cmd --help" in a pipeline or a CI job.
# @stress timeout=900
. "$STRESS_ROOT/lib/check.sh"

# Commands where --help is an operand in bash or makes no sense.
exempt=" . : [ true false echo printf test break continue return exit logout shift eval exec source
         times wait fg bg suspend set unset export readonly local declare typeset let builtin command
         __ctrlc __fzf __histstat __lineedit __mksock __predict __revsearch "

slow=""
hung=""
nohelp=""
noman=""
count=0
for c in $(compgen -b | sort -u); do
    case "$exempt" in *" $c "*) continue ;; esac
    count=$(( count + 1 ))
    start=$EPOCHREALTIME
    timeout 10 "$STRESS_WBSH" -r -c "$c --help" > h.out 2>&1 < /dev/null
    rc=$?
    ms=$(( (${EPOCHREALTIME/./} - ${start/./}) / 1000 ))
    if [ "$rc" -eq 124 ]; then
        hung="$hung $c"
        continue
    fi
    [ "$ms" -gt 2000 ] && slow="$slow $c(${ms}ms)"
    if [ "$rc" -ne 0 ] || [ ! -s h.out ]; then
        nohelp="$nohelp $c(rc=$rc)"
    fi
    if ! "$STRESS_WBSH" -r -c "whatis $c" > /dev/null 2>&1 < /dev/null; then
        noman="$noman $c"
    fi
done
note "$count commands checked"

severity high
expect_eq "no command hangs on --help" "${hung# }" ""
severity medium
expect_eq "every command prints help and exits 0 for --help" "${nohelp# }" ""
expect_eq "every command answers --help within 2 s" "${slow# }" ""
severity low
expect_eq "every command has a whatis / man entry" "${noman# }" ""
finish
