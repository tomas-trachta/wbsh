#!/bin/bash
# Windows programs behave the way a Windows user expects.
. "$STRESS_ROOT/lib/check.sh"

severity medium
wbsh_run 'chcp'
expect_match "chcp (chcp.com, a .COM program on PATHEXT) is found" "$RUN_OUT" "*: [0-9]*"
wbsh_run 'where where'
expect_match "where.exe runs by bare name" "$RUN_OUT" "*where.exe*"
printf '@echo from-cmd %%1\r\n@exit /b 3\r\n' > t.cmd
wbsh_run './t.cmd arg | tr -d "\r"; echo "rc=${PIPESTATUS[0]}"'
expect_eq "a .cmd file runs and its exit code arrives" "$RUN_OUT" "$(printf 'from-cmd arg\nrc=3')"
wbsh_run 't; echo "rc=$?"'
expect_match "a .cmd file in the current folder is not found by bare name" "$RUN_OUT" "*rc=127*"
wbsh_run 'cmd /c "exit 5"; echo "rc=$?"'
expect_eq "native exit code arrives" "$RUN_OUT" "rc=5"
wbsh_run 'powershell -NoProfile -Command "Write-Output ok" 2>&1 | tr -d "\r"'
expect_eq "powershell output through a pipe" "$RUN_OUT" ok
wbsh_run 'echo abc | findstr b | tr -d "\r"'
expect_eq "native filter in a pipeline" "$RUN_OUT" abc
wbsh_run 'cd /c/Windows && pwd'
expect_eq "POSIX drive paths work for cd" "$RUN_OUT" "/c/Windows"
wbsh_run 'cd "C:\Windows" && pwd'
expect_eq "Windows paths work for cd" "$RUN_OUT" "/c/Windows"
finish
