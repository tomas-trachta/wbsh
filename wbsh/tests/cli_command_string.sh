#!/bin/bash
# `wbsh -c` runs the command without -r, as `$SHELL -c` callers expect; a
# dump flag without -r still only dumps. A file still needs -r (#291).
# -l/--login and combined short options (-lc) are accepted, as omp and
# other `$SHELL -l -c` callers pass them.

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=../../x64/Debug/wbsh.exe

echo "=== -c runs"
"$WBSH" -c 'echo hi'
"$WBSH" -c 'echo "0=$0 1=$1 2=$2"' name one two
"$WBSH" -c 'exit 3'; echo "status $?"
"$WBSH" -r -c 'echo with -r'

echo "=== a dump flag without -r only dumps"
"$WBSH" --ast -c 'echo not run'
"$WBSH" -t -c 'echo not run'
"$WBSH" -e -c 'echo not run'

echo "=== -r with a dump flag dumps and runs"
"$WBSH" -r --ast -c 'echo ran'

echo "=== a file without -r is dumped, with -r it runs"
mkdir -p _run_tmp
echo 'echo from the file' > _run_tmp/cli_file.sh
"$WBSH" _run_tmp/cli_file.sh
"$WBSH" -r _run_tmp/cli_file.sh
rm -f _run_tmp/cli_file.sh

# -l reads ~/.wbsh_profile: keep the tester's own profile out of the output.
mkdir -p _run_tmp/login_home
export HOME="$PWD/_run_tmp/login_home"

echo "=== short options combine, as in bash"
"$WBSH" -lc 'echo "0=$0 1=$1"' name one
"$WBSH" -cl 'echo -c first in the group'
"$WBSH" -rlc 'echo -r too'
"$WBSH" -lx -c 'echo not run'; echo "status $?"

echo "=== -l makes a login shell that reads ~/.wbsh_profile"
echo 'echo profile ran; FROM_PROFILE=yes' > "$HOME/.wbsh_profile"
"$WBSH" -l -c 'echo "FROM_PROFILE=$FROM_PROFILE"; shopt login_shell'
"$WBSH" --login -c 'echo "--login works"'
"$WBSH" -c 'echo "FROM_PROFILE=$FROM_PROFILE"; shopt login_shell'
"$WBSH" -c 'shopt -s login_shell; echo "status $?"; shopt login_shell'

echo "=== exit in the profile ends the shell"
echo 'exit 7' > "$HOME/.wbsh_profile"
"$WBSH" -lc 'echo not run'; echo "status $?"
rm -f "$HOME/.wbsh_profile"

echo "=== a subshell of a login shell is a login shell too"
"$WBSH" -lc '(shopt -q login_shell) && echo yes'

echo "=== logout ends a login shell only"
"$WBSH" -lc 'logout 4; echo not run'; echo "status $?"
"$WBSH" -c 'logout'; echo "status $?"
rm -rf _run_tmp/login_home
