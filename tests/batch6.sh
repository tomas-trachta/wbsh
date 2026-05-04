#!/bin/bash
echo "=== md5/sha hashes"
echo -n hello | md5sum
echo -n hello | sha1sum
echo -n hello | sha256sum
echo -n abc   | sha512sum

echo "=== base64"
echo -n "Hello, World!" | base64
echo -n "SGVsbG8sIFdvcmxkIQ==" | base64 -d
echo

echo "=== grep -r"
mkdir -p /tmp/wbsh-grep/sub
printf 'apple\nbanana\n' > /tmp/wbsh-grep/a.txt
printf 'orange\nbanana\n' > /tmp/wbsh-grep/sub/b.txt
grep -rn banana /tmp/wbsh-grep | sort
rm -rf /tmp/wbsh-grep

echo "=== git completion engine (manual probe)"
# We can't drive the line editor in piped mode, but we can at least show
# that the underlying branch list works by inspecting `.git`.
( cd /tmp && rm -rf wbsh-git-c && mkdir wbsh-git-c && cd wbsh-git-c
  git init -b main >/dev/null 2>&1
  git -c user.email=t@t -c user.name=t commit --allow-empty -m init >/dev/null 2>&1
  git checkout -b feature-A >/dev/null 2>&1
  git checkout -b feature-B >/dev/null 2>&1
  ls .git/refs/heads )

rm -rf /tmp/wbsh-git-c

echo "=== done"
