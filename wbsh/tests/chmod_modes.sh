#!/bin/bash
# chmod's octal and symbolic modes and the one permission model test -x,
# [[ -x ]], ls -l, stat and find share (#269): the owner write bit is the
# read-only attribute, the execute bits default from the name and content
# and otherwise follow what chmod set. Everything happens inside a fresh
# temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

modes() {
    local f
    for f in "$@"; do
        printf '%-10s %s %s ' "$f" "$(stat -c '%a %A' "$f")" "$(ls -ld "$f" | cut -d' ' -f1)"
        [ -x "$f" ] && printf 'x ' || printf -- '- '
        [[ -x $f ]] && printf 'x ' || printf -- '- '
        [ -w "$f" ] && echo w || echo -
    done
}

touch plain data.txt
printf 'echo deploy\n' > deploy.sh
printf '#!/bin/sh\necho tool\n' > tool
touch run.exe
mkdir dir

echo "=== defaults"
modes plain data.txt deploy.sh tool run.exe dir

echo "=== +x / -x"
chmod +x plain
chmod -x deploy.sh tool run.exe
modes plain deploy.sh tool run.exe
chmod u+x deploy.sh
modes deploy.sh

echo "=== octal"
for m in 755 0644 600 0700 4755 444 7; do
    chmod $m data.txt
    modes data.txt
done
chmod 644 data.txt

echo "=== symbolic"
for m in a=rx u+w go-w a=r u+w,g+w ugo-w u=rwx,g=rx,o= g=u o=g u-x,a+X =rw +w; do
    chmod $m data.txt
    printf '%-14s ' "$m"
    modes data.txt
done

echo "=== X sets execute only on directories and executables"
chmod 644 plain data.txt
chmod 755 tool
mkdir -p tree/sub
touch tree/a tree/sub/b
chmod -R a-x,a+X tree plain tool
modes tree tree/sub tree/a tree/sub/b plain tool

echo "=== -R"
chmod -R go-rx tree
modes tree tree/sub tree/sub/b
chmod -R 755 tree
modes tree/a tree/sub/b

echo "=== -v / -c / --reference"
chmod -v 640 plain
chmod -v 640 plain
chmod -c 600 plain
chmod -c 600 plain
chmod --reference=tool plain
modes plain

echo "=== the modification time is kept"
touch -d '2020-01-02 03:04:05' stamp
before=$(stat -c %Y stamp)
chmod +x stamp
chmod 400 stamp
chmod u+w,-x stamp
[ "$(stat -c %Y stamp)" = "$before" ] && echo "mtime kept"
stat -c %a stamp

echo "=== the umask limits clauses without u/g/o/a"
umask 077
touch masked
chmod 644 masked
chmod +x masked
chmod -r masked
stat -c %a masked
umask 022

echo "=== copies and renames keep the mode"
chmod 750 plain
cp plain copy
mv plain moved
modes copy moved

echo "=== find sees the same bits"
find . -maxdepth 1 -type f -perm -u+x | sort
find . -maxdepth 1 -type f -executable | sort

echo "=== a script made executable runs and passes -x"
printf 'echo ran\n' > job
[ -x job ] || echo "job not executable yet"
chmod +x job && [ -x job ] && ./job

echo "=== errors"
chmod 2> /dev/null; echo "no operands: $?"
chmod 644 2> /dev/null; echo "no file: $?"
chmod rwx data.txt; echo "bad mode: $?"
chmod 9 data.txt; echo "bad octal: $?"
chmod u+q data.txt; echo "bad letter: $?"
chmod 644 missing; echo "missing: $?"
chmod --bogus 644 data.txt; echo "bad option: $?"

cd /
rm -rf "$D"
