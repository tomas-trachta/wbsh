#!/bin/bash
# The \g prompt must not run commands a repository's own .git/config names:
# git status runs core.fsmonitor and the clean filters of changed files, and
# an unpacked archive can bring any .git/config. Such repositories show the
# branch only; ordinary ones keep their state.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
p='\g'
marker="$D/ran"

g() {
    git -c user.name=t -c user.email=t@example.com -c core.autocrlf=false "$@" > /dev/null 2>&1
}

echo "=== core.fsmonitor in .git/config does not run"
mkdir fsmon && cd fsmon
g init -q . && g symbolic-ref HEAD refs/heads/main
echo x > a.txt
g config core.fsmonitor "echo ran > '$marker'; false"
echo "[${p@P}]"
[ -e "$marker" ] && echo "fsmonitor ran" || echo "fsmonitor did not run"
cd ..

echo "=== a clean filter in .git/config does not run"
mkdir filter && cd filter
g init -q . && g symbolic-ref HEAD refs/heads/main
printf '*.txt filter=evil\n' > .gitattributes
echo one > a.txt
g add . && g commit -qm one
g config filter.evil.clean "sh -c 'echo ran > \"$marker\"; cat'"
echo two > a.txt
echo "[${p@P}]"
[ -e "$marker" ] && echo "filter ran" || echo "filter did not run"
cd ..

echo "=== an include in .git/config also turns the state off"
mkdir include && cd include
g init -q . && g symbolic-ref HEAD refs/heads/main
echo x > a.txt
g config include.path ../other.config
echo "[${p@P}]"
cd ..

echo "=== an ordinary repository keeps its state"
mkdir plain && cd plain
g init -q . && g symbolic-ref HEAD refs/heads/main
echo x > a.txt
echo "[${p@P}]"

cd / && rm -rf "$D"
