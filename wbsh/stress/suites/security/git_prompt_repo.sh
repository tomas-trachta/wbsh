#!/bin/bash
# Entering a hostile repository must not run its code through the prompt.
# The \g prompt escape runs `git status` in the current repository. A
# .git/config from an unpacked archive can set core.fsmonitor (or
# core.hooksPath, diff/filter drivers) to any command, and git status runs
# core.fsmonitor. Prompt tools in other shells fixed this by passing
# -c core.fsmonitor= to git, or by not running git in untrusted folders.
. "$STRESS_ROOT/lib/check.sh"

if ! command -v git > /dev/null 2>&1; then
    skip "git not installed"
    finish
fi

mkdir repo && cd repo || exit 1
git init -q . > /dev/null 2>&1
echo x > a.txt
marker="$STRESS_TMP/fsmonitor-ran"
git config core.fsmonitor "echo ran > '$marker'; false"
p='\g'

severity high
out="${p@P}"
note "prompt text: $out"
check "the \g prompt does not run core.fsmonitor from .git/config" [ ! -e "$marker" ]
rm -f "$marker"
git config --unset core.fsmonitor
git config core.untrackedCache true
out="${p@P}"
check "control: the prompt still works without the hostile setting" [ -n "$out" ]
finish
