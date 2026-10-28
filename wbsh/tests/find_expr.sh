#!/bin/bash
# find: GNU expression language (operators, tests, actions, positional
# options), -maxdepth counted from the starting point, link handling with
# -P / -H / -L. Output was checked against GNU findutils 4.9.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
mkdir -p a/b/c other
printf 'hello\n' > a/x.txt
printf '' > a/empty
printf '%2000s' x > a/b/big
printf 'ab' > a/b/Two.TXT
echo g > other/g.txt

echo "=== depth limits"
find . -maxdepth 0
find . -maxdepth 1 | sort
find . -mindepth 3 | sort
find a -mindepth 1 -maxdepth 1 -type d

echo "=== names and paths"
find . -name '*.txt' | sort
find . -iname '*.txt' | sort
find . -path './a/b/*' | sort
find . -ipath '*/B/two*'
find a/ -name x.txt
find a// -maxdepth 1 -name empty

echo "=== operators"
find . -type d -name c -o -type f -name x.txt | sort
find a ! -type d | sort
find a -not -name '*.*' -type f | sort
find a \( -name x.txt -o -name empty \) -print | sort
find a -name x.txt -print , -name empty -printf 'comma %f\n' | sort
find . -name b -prune -o -type f -print | sort

echo "=== types, sizes, emptiness"
find a -type f,d -name 'e*'
find a -size +1k
find a -size 2c
find a -size -1k -type f
find a -size 4
find a -empty | sort

echo "=== times"
sleep 1
touch a/newer
find a -name newer -newer a/x.txt
find a -name x.txt -newer a/newer
find a/x.txt ! -newer a/newer
find a/x.txt -mtime 0
find a/x.txt -mtime +0
find a/x.txt -mmin -10
find a -newermt '2000-01-01 00:00' -name x.txt
find a -newermt '2999-01-01' -name x.txt
rm a/newer

echo "=== regex and perm"
find a -regex '.*/\(big\|empty\)' | sort
find a -regextype posix-extended -regex '.*/(big|empty)' | sort
find a -type f -perm 644 -name x.txt
find a -type d -perm -u=rwx -name c
chmod a-w a/x.txt
find a -type f ! -perm -u+w
find a -type f ! -writable
chmod u+w a/x.txt

echo "=== print0, printf, fprint"
find a -name '*.txt' -print0 | tr '\0' '|'
echo
find . -maxdepth 2 -printf '%p|%f|%h|%P|%H|%d|%y|%m\n' | sort
find a/x.txt -printf '%s %-6f| %5s|\n'
find a/x.txt -printf 'tab\there\\n %% \101\n'
find a -name empty -fprint list.txt
cat list.txt
rm list.txt

echo "=== exec"
find a -name '*.txt' -exec echo found {} \; | sort
find a -name '*.txt' -exec echo batch {} +
find a -name x.txt -exec echo {}.bak \;
find a -name big -execdir pwd \; | sed 's|.*/||'
find a -type f -execdir echo dir {} + | sort
find a -name x.txt -exec test -s {} \; -print
find a -name x.txt -ok echo yes {} \; < /dev/null
echo
find a -name x.txt -exec false {} +
echo "failing batch: $?"
find a -name x.txt -exec nosuchcommand {} \;
echo "missing command: $?"

echo "=== quit"
find a -name '*' -print -quit

echo "=== links"
cmd /c mklink /J "a\\b\\loop" a > /dev/null
cmd /c mklink /J "a\\side" other > /dev/null
find a -name g.txt
find -L a -name g.txt 2>&1
echo "loop status: $?"
find a -type l | sort
find -H a/side
find a -xtype d -name side

echo "=== delete"
mkdir -p del/keep del/gone
echo z > del/keep/z
echo y > del/gone/y
find del -name gone -delete
echo "non-empty: $?"
find del -path '*gone*' -delete
find del | sort
find del -name keep -prune -delete
echo "prune with delete: $?"
find del -depth -delete
[ -e del ] || echo "del removed"

echo "=== errors"
find nonexistent
echo "rc=$?"
find . -bogus
echo "rc=$?"
find . -name
echo "rc=$?"
find . -size 3q
echo "rc=$?"
find . -type z
echo "rc=$?"
find . \( -name a
echo "rc=$?"
find . -o -name a
echo "rc=$?"
find . -maxdepth x
echo "rc=$?"
find . -exec echo
echo "rc=$?"

cd /
rm -rf "$D"
