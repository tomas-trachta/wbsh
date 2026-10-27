#!/bin/bash
# Probe: a builtin, function or compound command in a multi-command pipeline (or run with &)
# sees the shell's positionals, $0, $?, $$, set options and shopt flags (bash 5.2 semantics).

mkdir -p _run_tmp

echo "=== 1. positional parameters and \$0"
f() { echo "in f: $1 $#" | cat; }; f hello world
set -- a b c; echo "$2 $#" | cat
g() { echo "0=$0 1=$1"; }; g x | cat
{ echo "all: $*"; } | cat

echo "=== 2. \$? of the command before the pipeline"
false; echo "status $?" | cat
(exit 7); { echo "brace $?"; } | cat
h() { echo "h sees $? and $1"; }; false; h arg | cat
true; echo "after true $?" | cat
false; echo "bg $?" > _run_tmp/pipeline_bg.txt & wait; cat _run_tmp/pipeline_bg.txt

echo "=== 3. \$\$ is the parent shell's pid"
[ "$$" = "$(echo $$ | cat)" ] && echo "same \$\$" || echo "different \$\$"
[ "$$" != "$({ echo $BASHPID; } | cat)" ] && echo "own \$BASHPID" || echo "parent's \$BASHPID"

echo "=== 4. set options"
set -o pipefail; { set -o | grep -E '^pipefail' | tr -s ' \t' ' '; } | cat; set +o pipefail
set -f; { echo "flags=$-"; } | cat; set +f
set -u; { echo "${nope-default}"; } | cat; set +u

echo "=== 5. shopt flags"
shopt -s nullglob; { echo "nullglob:" *.nomatch "(none)"; } | cat; shopt -u nullglob
shopt -s extglob; { shopt -q extglob && echo "extglob on"; } | cat; shopt -u extglob
shopt -s dotglob; { shopt -q dotglob && echo "dotglob on" || echo "dotglob off"; } | cat; shopt -u dotglob
shopt -u globstar; { shopt -q globstar && echo "globstar on" || echo "globstar off"; } | cat; shopt -s globstar
{ shopt -q nullglob && echo "nullglob leaked" || echo "defaults back"; } | cat
