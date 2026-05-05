#!/bin/bash
# Smoke test for the filesystem coreutils. Uses a local _fs_tmp/ to keep
# output deterministic across machines.

D=_fs_tmp
rm -rf $D 2>/dev/null
mkdir -p $D

echo "=== mkdir / rmdir"
mkdir $D/a
mkdir -p $D/a/b/c
ls $D/a
ls $D/a/b
rmdir $D/a/b/c
ls $D/a/b

echo "=== touch / size tests"
touch $D/empty.txt
[ -f $D/empty.txt ] && echo "exists empty"
printf 'hello' > $D/hello.txt
[ -s $D/hello.txt ] && echo "non-empty hello"

echo "=== cp"
cp $D/hello.txt $D/hello-copy.txt
cat $D/hello-copy.txt
echo
cp -r $D/a $D/a-copy
ls $D/a-copy
ls $D/a-copy/b

echo "=== mv"
mv $D/hello-copy.txt $D/hello-renamed.txt
[ -e $D/hello-copy.txt ] || echo "copy gone"
[ -e $D/hello-renamed.txt ] && echo "renamed exists"

echo "=== rm"
rm $D/hello-renamed.txt
[ -e $D/hello-renamed.txt ] || echo "removed"
rm -rf $D/a-copy
[ -d $D/a-copy ] || echo "tree removed"

echo "=== ls -1 / ls -a"
mkdir $D/ls
touch $D/ls/.hidden $D/ls/visible.txt $D/ls/another.txt
ls -1 $D/ls | sort
echo --
ls -1a $D/ls | sort

echo "=== test -e / -f / -d / -r"
[ -e $D ] && echo "dir exists"
[ -d $D ] && echo "is dir"
[ -f $D/hello.txt ] && echo "is file"
[ -r $D/hello.txt ] && echo "readable"
[ ! -e $D/nonexistent ] && echo "missing ok"

echo "=== find"
find $D -name "*.txt" | sort

rm -rf $D 2>/dev/null
echo "=== done"
