#!/bin/bash
# cpio -H: every format it writes (newc, crc, odc, bin, ustar, tar) is read
# back by -t / -i; header bytes for fixed dates; crc checksums verified on
# extraction; ustar's name prefix and the old tar name limit.

D=_cpf_tmp
rm -rf $D 2>/dev/null
mkdir -p $D
cd $D || exit 1
mkdir -p tree/sub
printf 'hello\n' > tree/a.txt
printf 'deep file\n' > tree/sub/b.txt
touch -d @981173106 tree/a.txt tree/sub/b.txt

echo "=== round trips"
for format in newc crc odc bin ustar tar; do
    echo "--- $format"
    find tree | sort | cpio -o -H $format > t.$format 2> /dev/null
    wc -c < t.$format
    cpio -t < t.$format
    rm -rf out
    mkdir out
    (cd out && cpio -idv < ../t.$format)
    cat out/tree/a.txt out/tree/sub/b.txt
done

echo "=== format option spellings"
echo tree/a.txt | cpio -o -Hodc 2> /dev/null > t1.odc
echo tree/a.txt | cpio -o --format odc 2> /dev/null | cmp - t1.odc && echo "same bytes"
echo tree/a.txt | cpio -o --format=odc 2> /dev/null | cmp - t1.odc && echo "same bytes"
echo tree/a.txt | cpio -oc 2> /dev/null | head -c 6; echo

echo "=== headers"
for format in newc crc odc; do
    echo "--- $format"
    echo tree/a.txt | cpio -o -H $format 2> /dev/null | head -c 130 | od -An -c
done
echo "--- bin"
echo tree/a.txt | cpio -o -H bin 2> /dev/null | head -c 48 | od -An -tx1
echo "--- ustar"
echo tree/a.txt | cpio -o -H ustar 2> /dev/null | head -c 512 | od -An -c
echo "--- tar"
echo tree/a.txt | cpio -o -H tar 2> /dev/null | head -c 160 | od -An -c

echo "=== crc verification"
echo tree/a.txt | cpio -o -H crc > bad.crc 2> /dev/null
printf 'J' | dd of=bad.crc bs=1 seek=124 conv=notrunc status=none
mkdir crc_out
(cd crc_out && cpio -id < ../bad.crc; echo "status $?"; cat tree/a.txt)
mkdir newc_out
echo tree/a.txt | cpio -o -H newc > bad.newc 2> /dev/null
printf 'J' | dd of=bad.newc bs=1 seek=124 conv=notrunc status=none
(cd newc_out && cpio -id < ../bad.newc 2> /dev/null; echo "status $?"; cat tree/a.txt)

echo "=== long names"
deep=tree/$(printf '%060d' 0)
mkdir -p $deep
printf 'far down\n' > $deep/$(printf '%050d' 1).txt
find tree -name '*.txt' | sort > list
cpio -o -H ustar < list > long.ustar 2> /dev/null
cpio -t < long.ustar 2> /dev/null | awk '{ print length($0) }'
mkdir long_out
(cd long_out && cpio -id < ../long.ustar 2> /dev/null && cat $deep/*.txt)
cpio -o -H tar < list > long.tar 2> err
sed 's/[0-9]\{20,\}/N/g' err
cpio -t < long.tar 2> /dev/null

echo "=== errors"
echo tree/a.txt | cpio -o -H zip > /dev/null; echo "status $?"
echo nothere | cpio -o -H odc > none.odc; echo "status $?"
cpio -t < none.odc; echo "status $?"
printf 'not an archive at all' | cpio -t; echo "status $?"

cd ..
rm -rf $D
echo "=== done"
