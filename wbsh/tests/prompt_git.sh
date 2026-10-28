#!/bin/bash
# The prompt's \g: branch, an operation in progress, and the work tree's
# state from `git status` -- which wbsh runs directly, without cmd.exe.
# ${p@P} expands a prompt string the way PS1 is expanded; outside an
# interactive terminal it waits for `git status` to finish.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
p='\g'

g() {
    git -c user.name=t -c user.email=t@example.com -c core.autocrlf=false "$@" > /dev/null 2>&1
}

g init -q .
g symbolic-ref HEAD refs/heads/main

echo "=== a fresh repository with an untracked file"
echo one > a.txt
echo "[${p@P}]"

echo "=== staged, then committed and clean"
g add a.txt
echo "[${p@P}]"
g commit -q -m first
echo "[${p@P}]"

echo "=== unstaged changes outrank staged and untracked ones"
echo two >> a.txt
echo new > b.txt
echo "[${p@P}]"
g add b.txt
echo "[${p@P}]"
g checkout -q -- a.txt
echo "[${p@P}]"

echo "=== an operation in progress is shown instead"
touch .git/MERGE_HEAD
echo "[${p@P}]"
rm .git/MERGE_HEAD

echo "=== in a subdirectory, on another branch"
mkdir -p sub/dir
cd sub/dir || exit 1
g checkout -q -b feature/x
echo "[${p@P}]"
cd "$D" || exit 1

echo "=== WBSH_GIT_NO_DIRTY leaves the state out"
WBSH_GIT_NO_DIRTY=1
echo "[${p@P}]"
unset WBSH_GIT_NO_DIRTY

echo "=== a detached HEAD"
g checkout -q --detach
p2="${p@P}"
case "$p2" in
    *"(detached)"*) echo "detached shown" ;;
    *) echo "detached missing: $p2" ;;
esac

cd /
rm -rf "$D"
