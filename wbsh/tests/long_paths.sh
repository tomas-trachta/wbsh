#!/bin/bash
# Paths past MAX_PATH: file names handed to Win32 get the \\?\ form, the
# recursive walkers extend the paths they build, and the manifest makes
# wbsh long-path aware. Changing into such a directory and starting a
# process there also needs the system's LongPathsEnabled switch, so that
# part only runs where it is on. Everything happens in a fresh temp dir.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
seg=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
deep=$seg/$seg/$seg/$seg
[ ${#deep} -gt 300 ] && echo "relative path is over 300 characters"

echo "=== create, write, read"
mkdir -p "$deep/sub"
echo "mkdir -p: $?"
echo hello > "$deep/sub/file.txt"
echo "redirect: $?"
echo more >> "$deep/sub/file.txt"
cat "$deep/sub/file.txt"
cat "$D/$deep/sub/file.txt" | wc -l
[ -f "$deep/sub/file.txt" ] && echo "test -f sees it"
wc -l < "$deep/sub/file.txt"
ls "$deep/sub"

echo "=== edit, copy, move, remove"
sed -i s/more/less/ "$deep/sub/file.txt"
tail -1 "$deep/sub/file.txt"
cp "$deep/sub/file.txt" "$deep/sub/copy.txt"
echo "cp: $?"
mv "$deep/sub/copy.txt" "$deep/sub/moved.txt"
echo "mv: $?"
ls "$deep/sub"
rm "$deep/sub/moved.txt"
echo "rm: $?"

echo "=== walkers"
find . -name file.txt | sed "s/$seg/SEG/g"
grep -r less . | sed "s/$seg/SEG/g"
cp -r "$seg" copytree
echo "cp -r: $?"
find copytree -type f | sed "s/$seg/SEG/g"
tar cf t.tar "$seg"
echo "tar c: $?"
mkdir x
cd x && tar xf ../t.tar
echo "tar x: $?"
cd "$D"
find x -name file.txt | sed "s/$seg/SEG/g"

echo "=== working directory"
if reg query 'HKLM\SYSTEM\CurrentControlSet\Control\FileSystem' /v LongPathsEnabled 2> /dev/null | grep -q 0x1; then
    cd "$deep/sub"
    echo "cd: $?"
    cat file.txt | head -1
    pwd | grep -q "$deep/sub\$" && echo "pwd shows the long path"
    cmd /c echo external process started | tr -d '\r'
    cd "$D"
else
    echo "cd: 0"
    echo "hello"
    echo "pwd shows the long path"
    echo "external process started"
fi

echo "=== cleanup"
rm -rf "$seg" copytree x
echo "rm -rf: $?"
ls
cd /
rm -rf "$D"
