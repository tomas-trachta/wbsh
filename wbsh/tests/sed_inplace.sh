#!/bin/bash
# sed -i writes a temporary file next to the target and renames it over the
# original, so a failure leaves the original untouched; backups, read-only
# targets and non-regular operands behave as in GNU sed.
# Everything happens inside a fresh temporary directory.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
printf 'a\nb\n' > f

echo "=== plain edit"
sed -i s/a/X/ f
echo "rc=$?"
cat f

echo "=== backup suffix"
sed -i.orig s/b/Y/ f
echo "rc=$?"
cat f.orig f

echo "=== backup with * goes where the suffix says"
mkdir bak
sed -i'bak/*.old' s/X/Z/ f
echo "rc=$?"
ls bak
cat bak/f.old f

echo "=== --in-place=SUFFIX"
printf 'x\n' > g
sed --in-place=.b s/x/y/ g
cat g.b g

echo "=== several files, one missing"
printf '1\n' > h
sed -i s/1/2/ h nofile g
echo "rc=$?"
cat h g

echo "=== a directory is not editable, - is a file name, stdin is not used"
mkdir d
sed -i s/a/b/ d
echo "rc=$?"
sed -i s/a/b/ - < f
echo "rc=$?"
echo a | sed -i s/a/b/
echo "rc=$?"

echo "=== a read-only file is replaced and stays read-only"
printf 'ro\n' > ro
chmod a-w ro
sed -i s/ro/rw/ ro
echo "rc=$?"
cat ro
[ -w ro ] || echo "still read-only"
chmod u+w ro

echo "=== no temporary files are left behind"
ls

echo "=== the original survives when the temporary file cannot be created"
mkdir locked
printf 'keep\n' > locked/k
icacls locked /deny "$USERNAME:(AD,WD)" > /dev/null
sed -i s/keep/lost/ locked/k 2>&1 | sed 's/sed[A-Za-z0-9]\{6\}/sedXXXXXX/'
cat locked/k
icacls locked /remove:d "$USERNAME" > /dev/null
ls locked

cd /
rm -rf "$D"
