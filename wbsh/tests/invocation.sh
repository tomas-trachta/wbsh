#!/bin/bash
# The shell's own command line follows bash: -c takes the first operand
# after the options, set letters cluster, -o / -O take names, a copy named
# sh.exe starts in POSIX mode, and --ec carries a base64 command.

W=$BASH
T=_run_tmp/invocation
rm -rf "$T"
mkdir -p "$T"

echo "=== -c runs, operands are \$0 \$1 .."
"$W" -c 'echo "ran $0 $1 $#"' zero one
"$W" -c 'echo "no name: $#"'

echo "=== options before the command string"
"$W" -c -l 'shopt login_shell'
"$W" -ec 'false; echo not reached'
echo "status $?"
"$W" -eu -o pipefail -c 'echo "$SHELLOPTS"'
"$W" -xc 'echo traced' 2>&1
"$W" +B -c 'echo {a,b}'
"$W" -O extglob -c 'shopt extglob'
"$W" +O globstar -c 'shopt globstar'

echo "=== bad command lines exit 2"
"$W" -c 2>&1
echo "status $?"
"$W" -Z -c true 2>&1
echo "status $?"
"$W" -o no-such-option -c true 2>&1
echo "status $?"
"$W" -O no_such_shopt -c true 2>&1
echo "status $?"
"$W" --no-such-long-option 2>&1
echo "status $?"

echo "=== script files and stdin"
printf 'echo "script $# $*"\n' > "$T/s.sh"
"$W" "$T/s.sh" a b
"$W" -- "$T/s.sh" c
echo 'echo "stdin $# $1"' | "$W" -s x
echo 'echo "lone dash"' | "$W" -
echo 'echo "plain stdin"' | "$W"

echo "=== -n parses only"
"$W" -n -c 'echo should not run'
echo "status $?"
"$W" -n -c 'if then' 2>&1
echo "status $?"

echo "=== debugging dumps replace running unless -r"
"$W" --ast -c 'echo dumped' | head -2
"$W" -r --ast -c 'echo dumped and ran' | tail -1

echo "=== named sh / bash"
cp "$W" "$T/sh.exe"
cp "$W" "$T/bash.exe"
"$T/sh.exe" -c 'set -o | grep "^posix"'
"$T/bash.exe" -c 'set -o | grep "^posix"'

echo "=== startup files"
printf 'FROM=rcfile\n' > "$T/rc"
"$W" -i --rcfile "$T/rc" -c 'echo "with -i: ${FROM-none}"'
"$W" --rcfile "$T/rc" -c 'echo "without -i: ${FROM-none}"'
"$W" -i --norc --rcfile "$T/rc" -c 'echo "norc: ${FROM-none}"'
mkdir -p "$T/home"
printf 'FROM=profile\n' > "$T/home/.profile"
HOME="$PWD/$T/home" "$W" -l -c 'echo ".profile is not read: ${FROM-none}"'
printf 'FROM=wbsh_profile\n' > "$T/home/.wbsh_profile"
HOME="$PWD/$T/home" "$W" -l -c 'echo "login: ${FROM-none}"'
HOME="$PWD/$T/home" "$W" -l --noprofile -c 'echo "noprofile: ${FROM-none} $(shopt login_shell)"'
printf 'exit 7\n' > "$T/home/.wbsh_profile"
HOME="$PWD/$T/home" "$W" -l -c 'echo not reached'
echo "profile exit: $?"

echo "=== --ec"
"$W" --ec ZWNobyAiaGkgJDEiIHwgdHIgYS16IEEtWg== name arg
"$W" --ec ZWNobyAiaGkgJDEiIHwgdHIgYS16IEEtWg name unpadded
"$W" --EncodedCommand 'v1:ZWNobyBw
cmVmaXhlZA=='
"$W" --ec ZQBjAGgAbwAgAHUAdABmADEANgA=
"$W" --ec ZWNobyAidXJsLXNhZmUiID4-IC9kZXYvbnVsbDsgZWNobyBvaw
"$W" -eu --ec ZmFsc2U7IGVjaG8gbm90IHJlYWNoZWQ=
echo "status $?"
"$W" --ec 'not base64!' 2>&1
echo "status $?"

echo "=== BASHOPTS and SHELLOPTS"
"$W" -c 'case ":$BASHOPTS:" in *:checkwinsize:*) echo "checkwinsize listed" ;; esac'
"$W" -c 'shopt -s histappend; case ":$BASHOPTS:" in *:histappend:*) echo "histappend listed" ;; esac'
"$W" -c 'set -o noglob; case ":$SHELLOPTS:" in *:noglob:*) echo "noglob listed" ;; esac'

echo "=== \$- names how the commands arrived"
"$W" -c 'echo "$-"'
"$W" -euc 'echo "$-"'
echo 'echo "$-"' | "$W"
