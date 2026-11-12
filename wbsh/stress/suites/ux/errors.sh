#!/bin/bash
# Error messages and exit statuses users and scripts depend on.
. "$STRESS_ROOT/lib/check.sh"

severity high
wbsh_run 'nosuchcmd_xyz'
expect_eq "unknown command exits 127" "$RUN_RC" 127
expect_match "unknown command message names the command" "$RUN_ERR" "*nosuchcmd_xyz*not found*"
printf 'echo x\n' > noexec.txt
wbsh_run './noexec.txt; echo "rc=$?"'
note "running a non-executable text file: $RUN_OUT / $RUN_ERR"
mkdir -p adir
wbsh_run './adir'
expect_eq "running a directory exits 126" "$RUN_RC" 126
wbsh_run 'cd /no/such/dir'
expect_eq "cd to a missing directory exits 1" "$RUN_RC" 1
expect_match "cd message names the directory" "$RUN_ERR" "*/no/such/dir*"
wbsh_run 'if then fi'
expect_eq "syntax error exits 2" "$RUN_RC" 2
expect_match "syntax error message names the token" "$RUN_ERR" "*then*"
wbsh_run 'echo $(( 1 + ))'
check "arithmetic syntax error is an error" [ "$RUN_RC" -ne 0 ]
expect_match "arithmetic error message shows the expression" "$RUN_ERR" "*1 +*"

severity medium
wbsh_run 'ls --no-such-option-xyz'
check "unknown option exits non-zero" [ "$RUN_RC" -ne 0 ]
expect_match "unknown option message names the option" "$RUN_ERR" "*no-such-option-xyz*"
wbsh_run 'cat /no/such/file'
expect_match "missing file message names the file (GNU style)" "$RUN_ERR" "*/no/such/file*No such file or directory*"
wbsh_run 'x=1; readonly x; x=2'
check "assigning a readonly variable fails" [ "$RUN_RC" -ne 0 ]
expect_match "readonly message names the variable" "$RUN_ERR" "*x*readonly*"
wbsh_run 'set -u; echo $nope_var'
expect_match "set -u message names the variable" "$RUN_ERR" "*nope_var*"
wbsh_run 'f() { return 300; }; f; echo $?'
expect_eq "return status wraps to 8 bits" "$RUN_OUT" 44
wbsh_run 'exit 256'
expect_eq "exit status wraps to 8 bits" "$RUN_RC" 0
wbsh_run 'sleep 0.1 & wait $!; echo "wait=$?"; wait 99999; echo "bad=$?"'
expect_eq "wait on an unknown pid exits 127" "$RUN_OUT" "$(printf 'wait=0\nbad=127')"
finish
