#!/bin/bash
# Builtins that take no options still accept `--` as the end of options,
# as oh-my-bash's `eval -- "..."` relies on.

eval -- 'a=(x y)'; echo "eval: ${a[1]}"
f() { return -- 3; }; f; echo "return: $?"
let -- 'z = 2 + 3'; echo "let: $z"
set -- p q r; shift -- 2; echo "shift: $1"
for i in 1 2; do for j in 1 2; do break -- 2; done; done; echo "break: $i$j"
for i in 1 2; do for j in 1 2; do continue -- 2; done; done; echo "continue: $i$j"
OPTIND=1; getopts -- ab opt -a; echo "getopts: $opt"
g() { caller -- 0 >/dev/null; echo "caller: $?"; }; g
help -- eval >/dev/null; echo "help: $?"
mkdir -p _run_tmp/dd_dir
pushd -- _run_tmp/dd_dir >/dev/null && echo "pushd: ${PWD##*/}"
popd -- >/dev/null; echo "popd: $?"
( exit -- 4 ); echo "exit: $?"
( exec -- echo "exec: ok" )
rmdir _run_tmp/dd_dir
