#!/bin/bash
# Variable propagation across pipeline subshells, plus export gating.

echo "=== 1. Non-exported var visible in piped function"
prefix=">>"
greet=hi
emit() { echo "$prefix $greet from func"; }
emit
emit | tr a-z A-Z

echo "=== 2. Layered self-spawn: nested functions still see locals"
inner() { echo "(inner) prefix=$prefix greet=$greet"; }
outer() { inner; echo "(outer) prefix=$prefix greet=$greet"; }
outer
outer | wc -l

echo "=== 3. Externals do NOT see shell-local vars"
shell_only=visible-in-shell
echo "in-shell: $shell_only"
# cmd.exe prints %VAR% literally when VAR is undefined.
cmd /c "echo cmd-sees=%shell_only%"

echo "=== 4. Externals DO see exported vars"
export shared=in-env
cmd /c "echo cmd-shared=%shared%"

echo "=== 5. Export status survives a self-spawn round-trip"
report() {
  echo "report: shared=$shared (visible $([ -n \"$shared\" ] && echo yes || echo no))"
  echo "report: shell_only=$shell_only (visible $([ -n \"$shell_only\" ] && echo yes || echo no))"
}
report | sed 's/^/  /'

echo "=== 6. Var unset in pipeline subshell does not affect parent"
counter=42
clobber() { counter=99; echo "in child: $counter"; }
clobber | sed 's/^/  /'
echo "after pipeline: $counter"

echo "=== done"
