#!/bin/bash
# Smoke test for the archive coreutils (gzip family + tar + zip).

D=_arc_tmp
rm -rf $D 2>/dev/null
mkdir -p $D

echo "=== gzip / gunzip"
printf 'compress me, please. ' > $D/a.txt
printf 'compress me, please. ' >> $D/a.txt
printf 'compress me, please. ' >> $D/a.txt
cp $D/a.txt $D/b.txt
gzip $D/a.txt
[ -f $D/a.txt.gz ] && echo "gz file created"
[ -e $D/a.txt ] || echo "original removed"
gunzip $D/a.txt.gz
diff -q $D/a.txt $D/b.txt && echo "round-trip ok"

echo "=== zcat"
gzip -k $D/a.txt 2>/dev/null || gzip $D/a.txt
zcat $D/a.txt.gz
echo

echo "=== tar"
mkdir -p $D/src $D/dst
echo "f1" > $D/src/f1.txt
echo "f2" > $D/src/f2.txt
mkdir $D/src/sub
echo "f3" > $D/src/sub/f3.txt
( cd $D && tar -cf src.tar src )
( cd $D/dst && tar -xf ../src.tar )
diff -q $D/src/f1.txt $D/dst/src/f1.txt && echo "f1 ok"
diff -q $D/src/sub/f3.txt $D/dst/src/sub/f3.txt && echo "f3 ok"

echo "=== zip / unzip"
( cd $D && zip -q out.zip src/f1.txt src/f2.txt )
[ -f $D/out.zip ] && echo "zip created"
mkdir -p $D/uz
( cd $D/uz && unzip -q ../out.zip )
diff -q $D/src/f1.txt $D/uz/src/f1.txt && echo "unzip ok"

rm -rf $D 2>/dev/null
echo "=== done"
