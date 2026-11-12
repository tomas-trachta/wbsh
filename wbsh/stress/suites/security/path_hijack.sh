#!/bin/bash
# A bare command name must not run a file from the current directory.
# cmd.exe and CreateProcess search the current directory first; a POSIX
# shell must not. A user who cds into an untrusted folder (a fresh git
# clone, a download) and types a command must get the real program.
. "$STRESS_ROOT/lib/check.sh"

printf '@echo HIJACKED-BAT\r\n' > where.bat
printf '@echo HIJACKED-CMD\r\n' > where.cmd
printf '#!/bin/sh\necho HIJACKED-SCRIPT\n' > where
chmod +x where 2>/dev/null
printf '@echo HIJACKED-COM\r\n' > tasklist.bat

severity critical
wbsh_run 'where /?'
expect_no_match "'where' does not run where.bat / where.cmd / ./where from the current directory" "$RUN_OUT" "*HIJACKED*"
wbsh_run 'tasklist /FI "PID eq 0" /NH'
expect_no_match "'tasklist' does not run tasklist.bat from the current directory" "$RUN_OUT" "*HIJACKED*"
wbsh_run 'command -v where'
expect_match "command -v reports the System32 program" "$RUN_OUT" "*[Ss]ystem32*"
wbsh_run 'x=$(where /? 2>&1); echo "$x" | head -n 1'
expect_no_match "command substitution does not run the local file" "$RUN_OUT" "*HIJACKED*"
wbsh_run 'echo | xargs where /? 2>&1 | head -n 1'
expect_no_match "xargs does not run the local file" "$RUN_OUT" "*HIJACKED*"
wbsh_run 'find . -maxdepth 0 -exec where /? \; 2>&1 | head -n 1'
expect_no_match "find -exec does not run the local file" "$RUN_OUT" "*HIJACKED*"

severity low
wbsh_run 'PATH=".:$PATH"; where /?'
expect_match "control: with . in PATH the local file is found" "$RUN_OUT" "*HIJACKED*"
wbsh_run 'PATH="$PATH:"; where /? 2>&1 | head -n 1'
case "$RUN_OUT" in
    *HIJACKED*) review "an empty PATH element searches the current directory (POSIX does this; make sure the default PATH has no empty element)" ;;
esac
wbsh_run 'case ":$PATH:" in *::*|*:.:*) echo unsafe ;; *) echo clean ;; esac'
expect_eq "the default PATH has no empty or '.' element" "$RUN_OUT" clean
finish
