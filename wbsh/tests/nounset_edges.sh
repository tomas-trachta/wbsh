#!/bin/bash
# set -u: which expansions count as using an unset variable, as in bash 5.3.
# Each probe runs in a child shell so an error ends only the probe.

probe() {
    local out
    out=$("$WBSH" -c "set -u; $1"$'\n''echo "  reached, status $?"' 2>&1)
    printf '%s\n  => %s\n' "$1" "${out//$'\n'/ | }"
}

WBSH=../../x64/Release/wbsh.exe
[ -x "$WBSH" ] || WBSH=wbsh

probe 'echo "${X:=v} $X"'
probe 'echo "[${X:-}] [${X-}] [${X:+a}] [${X+a}]"'
probe 'echo "${#X}"'
probe 'echo "${X:?custom message}"'
probe 'echo "${X?}"'
probe 'echo "${X@Q}"'
probe 'echo "${X/a/b}"'
probe 'echo "${X:0:1}"'
probe 'x=y; echo "${!x}"'
probe 'x=y; echo "${!x:-dflt}"'
probe 'a=(); echo "[${a[@]}] [${a[*]}] ${#a[@]} [${!a[@]}]"'
probe 'a=(); echo "[${a[0]}]"'
probe 'a=(); echo "[${a[@]:0}] [${a[@]/x/y}]"'
probe 'unset b; echo "[${b[@]}] [${b[*]}] ${#b[@]}"'
probe 'declare -A m; echo "[${m[@]}] ${#m[@]}"'
probe 'declare -A m; echo "[${m[k]}]"'
probe 'declare -A m; echo "[${m[k]-d}]"'
probe 'echo "[$@] [$*] $# [${@:1}] [${*:-d}]"'
probe 'echo "$1"'
probe 'echo "${1-} ${1:-d}"'
probe 'echo "${#@} ${#*}"'
probe 'f() { echo "n=$#"; echo "$1"; }; f'
probe 'echo "$((X + 1))"'
probe '(( X )); echo "status $?"'
probe 'let X=X+1; echo "$X"'
probe '[[ -v X ]]; echo "status $?"'
probe 'for i in "${U[@]}"; do echo in; done; echo done'
probe 'f() { local v; echo "[$v]"; }; f'
probe 'f() { local v; echo "[${v-unset}]"; }; f'
probe 'declare v; echo "[$v]"'
probe 'echo "[${!NO_SUCH_PREFIX*}] [${!NO_SUCH_PREFIX@}]"'
probe 'echo "${X:-$Y}"'
probe ': "${X=}"; echo "[$X]"'
probe '[[ $BASH_MONOSECONDS$EPOCHSECONDS =~ ^[0-9]+$ ]] && echo dynamic variables are set'
