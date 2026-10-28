#!/bin/bash
# xz and lzma compress: .xz streams (every check type) and .lzma files
# round-trip through the bundled decoders, tar writes both, and the
# command line behaves like gzip's.

tmp=$(mktemp -d)
cd "$tmp" || exit 1

yes 'a line of compressible text' | head -c 1000000 > text.txt
awk 'BEGIN { srand(7); for (i = 0; i < 300000; i++) printf "%02x", int(rand() * 256) }' | xxd -r -p > noise.bin
printf 'x' > one.txt
: > empty.txt
cat text.txt noise.bin text.txt > mixed.bin

echo "=== round trips"
for f in empty.txt one.txt text.txt noise.bin mixed.bin; do
	xz -c $f | xz -dc | cmp - $f && echo "xz $f ok"
	lzma -c $f | lzma -dc | cmp - $f && echo "lzma $f ok"
done
echo hi | xz | xz -d
echo hi | lzma | unlzma

echo "=== headers"
xz -c empty.txt | wc -c
lzma -c empty.txt | wc -c
xz -c one.txt | od -An -tx1 -N12
lzma -c one.txt | od -An -tx1 -N13
lzma -0 -c one.txt | od -An -tx1 -N5

echo "=== sizes"
[ "$(xz -c text.txt | wc -c)" -lt 1000 ] && echo "repetitive input shrinks"
[ "$(xz -c noise.bin | wc -c)" -lt 301000 ] && echo "incompressible input stays near its size"
[ "$(xz -0 -c mixed.bin | wc -c)" -lt 305000 ] && echo "level 0 finds the repeats"

echo "=== levels and xz options"
for level in 0 1 2 3 4 5 6; do
	xz -$level -c mixed.bin | unxz | cmp - mixed.bin && echo "xz -$level ok"
done
xz -e -c mixed.bin | xzcat | cmp - mixed.bin && echo "xz -e ok"
xz -3e -c mixed.bin | xzcat | cmp - mixed.bin && echo "xz -3e ok"
lzma -1 -c mixed.bin | lzcat | cmp - mixed.bin && echo "lzma -1 ok"
xz -T0 -c one.txt | xzcat
echo
xz --threads=4 --extreme -2 -c one.txt | xzcat
echo

echo "=== check types"
for check in none crc32 crc64 sha256; do
	xz -C $check -c text.txt > c.xz
	xz -dc c.xz | cmp - text.txt && echo "$check ok"
	od -An -tx1 -j7 -N1 c.xz
done
xz --check=crc32 -c one.txt | od -An -tx1 -j7 -N1
xz -C md5 -c one.txt
echo "rc=$?"

echo "=== damage is caught by the check"
xz -c noise.bin > n.xz
{ head -c 5000 n.xz; printf 'garbage!'; tail -c +5009 n.xz; } > bad.xz
xz -t bad.xz
echo "rc=$?"
xz -C none -c noise.bin > n0.xz
{ head -c 5000 n0.xz; printf 'garbage!'; tail -c +5009 n0.xz; } > bad0.xz
xz -t bad0.xz
echo "rc=$? without a check"

echo "=== file handling"
cp text.txt f.txt
xz f.txt
ls f.txt*
xz f.txt.xz
echo "rc=$?"
unxz f.txt.xz
cmp f.txt text.txt && echo "unxz restored f.txt"
xz -k f.txt
ls f.txt*
xz -k f.txt
echo "rc=$?"
xz -kf f.txt && echo "forced over the old f.txt.xz"
xz -t f.txt.xz && echo "test ok"
xz -tv f.txt.xz
rm f.txt.xz
lzma f.txt
ls f.txt*
unlzma f.txt.lzma
cmp f.txt text.txt && echo "unlzma restored f.txt"
xz -F lzma -k f.txt
ls f.txt*
lzcat f.txt.lzma | cmp - text.txt && echo "xz -F lzma wrote .lzma"
xz -dc f.txt.lzma | cmp - text.txt && echo "xz reads .lzma too"
lzma --format=xz -k f.txt
ls f.txt*
xz -t f.txt.xz && echo "lzma --format=xz wrote .xz"
unxz -z -c one.txt | xzcat
echo

echo "=== bad options"
xz -F zip one.txt
echo "rc=$?"
xz --format=raw one.txt
echo "rc=$?"
xz -Q one.txt
echo "rc=$?"
gzip -e one.txt
echo "rc=$?"

echo "=== tar"
mkdir -p tree/sub
echo hello > tree/a.txt
cp text.txt tree/sub/text.txt
tar -cJf t.txz tree
xz -t t.txz && echo "tar -cJ wrote xz"
tar -tJf t.txz
tar --xz -cf t.tar.xz tree
tar -tf t.tar.xz | wc -l
tar --lzma -cf t.tar.lzma tree
lzma -t t.tar.lzma && echo "tar --lzma wrote lzma"
tar -tf t.tar.lzma | wc -l
tar -caf a.tar.xz tree
xz -t a.tar.xz && echo "tar -a picked xz"
tar -caf a.tar.lzma tree
lzma -t a.tar.lzma && echo "tar -a picked lzma"
mkdir out
tar -xJf t.txz -C out
cmp out/tree/sub/text.txt text.txt && echo "tar -xJ ok"
tar -cZf t.tar.Z tree
echo "rc=$?"

echo "=== streaming"
yes 'streamed line' | head -c 20000000 | xz -c | xz -dc | wc -c
yes | xz -c | xz -dc | head -2
yes | lzma -c | lzma -dc | head -2

cd / && rm -rf "$tmp"
