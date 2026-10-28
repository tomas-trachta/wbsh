#!/bin/bash
# BASH_VERSION, BASH_VERSINFO, BASH, PPID, OSTYPE, MACHTYPE, HOSTTYPE,
# EPOCHSECONDS, EPOCHREALTIME, BASH_COMMAND and WBSH_VERSION.

echo "=== version"
echo "BASH_VERSION=$BASH_VERSION"
echo "BASH_VERSINFO=${BASH_VERSINFO[*]}"
[ -n "$BASH_VERSION" ] || { echo "needs bash"; exit 1; }
(( BASH_VERSINFO[0] >= 4 )) && echo "version check ok"
[[ $WBSH_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] && echo "WBSH_VERSION looks like a version"
declare -p BASH_VERSINFO
BASH_VERSINFO[0]=9 2>/dev/null
echo "BASH_VERSINFO[0] still ${BASH_VERSINFO[0]}"

echo "=== platform"
echo "OSTYPE=$OSTYPE"
case $MACHTYPE in *-pc-msys) echo "MACHTYPE ok" ;; *) echo "MACHTYPE=$MACHTYPE" ;; esac
case $HOSTTYPE in x86_64|aarch64) echo "HOSTTYPE ok" ;; *) echo "HOSTTYPE=$HOSTTYPE" ;; esac
[[ $MACHTYPE == "$HOSTTYPE"-* ]] && echo "MACHTYPE starts with HOSTTYPE"
case $BASH in /*wbsh.exe) echo "BASH is this shell" ;; *) echo "BASH=$BASH" ;; esac

echo "=== PPID"
[[ $PPID =~ ^[1-9][0-9]*$ ]] && echo "PPID is a pid"
declare -p PPID | sed 's/"[0-9]*"/N/'
top=$PPID
( [ "$PPID" = "$top" ] && echo "subshell keeps PPID" )
[ "$(echo "$PPID")" = "$top" ] && echo "command substitution keeps PPID"
echo "$PPID" | { read -r p; [ "$p" = "$top" ] && echo "pipeline keeps PPID"; }
PPID=3 2>/dev/null
[ "$PPID" = "$top" ] && echo "PPID is readonly"
env | grep -c '^PPID=\|^BASH_VERSION=\|^OSTYPE='

echo "=== EPOCHSECONDS / EPOCHREALTIME"
[[ $EPOCHSECONDS =~ ^[0-9]{10,}$ ]] && echo "EPOCHSECONDS ok"
[[ $EPOCHREALTIME =~ ^[0-9]{10,}\.[0-9]{6}$ ]] && echo "EPOCHREALTIME ok"
a=$EPOCHSECONDS
b=$(date +%s)
(( b - a <= 2 && b >= a )) && echo "EPOCHSECONDS matches date +%s"
EPOCHSECONDS=5
(( EPOCHSECONDS > 5 )) && echo "assignment ignored"

echo "=== BASH_COMMAND"
trap 'echo "failed: $BASH_COMMAND"' ERR
test -d /nonexistent_bash_standard_vars
trap - ERR
echo "now: $BASH_COMMAND"
