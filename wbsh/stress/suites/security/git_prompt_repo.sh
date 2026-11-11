#!/bin/bash
# Entering a hostile repository must not run its code through the prompt.
# The \g prompt escape runs `git status` in the current repository. A
# .git/config from an unpacked archive can set core.fsmonitor or a clean
# filter to any command, and git status runs both.
. "$STRESS_ROOT/lib/check.sh"

if ! command -v git > /dev/null 2>&1; then
    skip "git not installed"
    finish
fi

p='\g'
marker="$STRESS_TMP/hostile-ran"

severity high
mkdir fsmon && cd fsmon || exit 1
git init -q . > /dev/null 2>&1
echo x > a.txt
git config core.fsmonitor "echo ran > '$marker'; false"
out="${p@P}"
note "prompt text with core.fsmonitor: $out"
check "the \\g prompt does not run core.fsmonitor from .git/config" [ ! -e "$marker" ]
rm -f "$marker"
cd ..

mkdir filter && cd filter || exit 1
git init -q . > /dev/null 2>&1
printf '*.txt filter=evil\n' > .gitattributes
echo one > a.txt
git -c user.name=t -c user.email=t@example.com add . > /dev/null 2>&1
git -c user.name=t -c user.email=t@example.com commit -qm one > /dev/null 2>&1
git config filter.evil.clean "sh -c 'echo ran > \"$marker\"; cat'"
echo two > a.txt
out="${p@P}"
note "prompt text with a clean filter: $out"
check "the \\g prompt does not run a clean filter from .git/config" [ ! -e "$marker" ]
rm -f "$marker"
cd ..

severity low
mkdir plain && cd plain || exit 1
git init -q . > /dev/null 2>&1
echo x > a.txt
out="${p@P}"
expect_match "control: an ordinary repository still shows its state" "$out" "*untracked*"
finish
