#!/bin/bash
# Script files in the forms people really have: CRLF, BOM, no newline, long lines.
# @stress timeout=60
. "$STRESS_ROOT/lib/check.sh"

run_file() {
    "$STRESS_WBSH" -r "$1" > out.txt 2> err.txt < /dev/null
    RUN_RC=$?
    RUN_OUT=$(cat out.txt)
    RUN_ERR=$(cat err.txt)
}

printf 'x=1\r\nif [ "$x" = 1 ]; then\r\n  echo crlf-ok\r\nfi\r\n' > crlf.sh
run_file crlf.sh
expect_eq "CRLF script runs" "$RUN_OUT" crlf-ok
printf '\xef\xbb\xbfecho bom-ok\n' > bom.sh
run_file bom.sh
severity low
expect_eq "UTF-8 BOM script runs (bash fails too, but Windows editors write a BOM)" "$RUN_OUT" bom-ok
severity medium
printf 'echo no-newline' > nonl.sh
run_file nonl.sh
expect_eq "script without a final newline runs" "$RUN_OUT" no-newline
printf 'echo %s | wc -c\n' "$(printf 'z%.0s' {1..200000})" > long.sh
run_file long.sh
expect_eq "200 kB script line" "$RUN_OUT" 200001
printf 'echo before\nexit 3\necho after\n' > ex.sh
run_file ex.sh
expect_eq "exit status of a script" "$RUN_OUT:$RUN_RC" "before:3"
printf 'echo start\nif true; then\n echo x\n' > broken.sh
run_file broken.sh
check "syntax error gives exit status 2" [ "$RUN_RC" -eq 2 ]
expect_match "syntax error message names the line" "$RUN_ERR" "*[0-9]*"
finish
