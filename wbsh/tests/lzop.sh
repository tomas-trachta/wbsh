#!/bin/bash
# lzop, unlzop and lzopcat: lzop 1.04 files of LZO1X-1 blocks, the header
# and block checksums checked on the way back, tar reading and writing them.

tmp=$(mktemp -d)
cd "$tmp" || exit 1

echo "=== a known input, byte for byte"
printf 'hello hello hello hello hello hello hello' > hello.txt
touch -d @1600000000 hello.txt
lzop hello.txt
od -An -tx1 hello.txt.lzo
ls hello.txt hello.txt.lzo
printf 'abc' | lzop -c | tail -c +39 | od -An -tx1
printf 'abc' | lzop -1 -c | head -c 25 | od -An -tx1
printf '' | lzop -c | tail -c 4 | od -An -tx1

echo "=== round trips"
roundtrip() {
    lzop -c "$1" > rt.lzo && lzop -dc rt.lzo | cmp - "$1" && echo "$2 ok"
}
: > empty
printf 'x' > one
yes 'repetitive line of text' | head -c 3000000 > repeat
head -c 1000000 /dev/urandom > random
head -c 262144 repeat > exact
head -c 262145 repeat > exact1
roundtrip empty "empty"
roundtrip one "one byte"
roundtrip repeat "repetitive, multi-block"
roundtrip random "incompressible"
roundtrip exact "exactly one block"
roundtrip exact1 "one block and a byte"
roundtrip "$BASH" "wbsh.exe"
lzop -1 -c random | lzop -dc | cmp - random && echo "-1 ok"
lzop -9 -C -c repeat | lzop -dc | cmp - repeat && echo "-9 -C ok"
lzop -F -c repeat | lzop -dc | cmp - repeat && echo "-F ok"
lzop -c random | wc -c | awk '{ print ($1 < 1000300) ? "stored blocks stay small" : "grew: " $1 }'
lzop -c repeat | wc -c
yes | lzop -c | lzopcat | head -2

echo "=== in place, names and options"
cp hello.txt a.txt
lzop -U a.txt
ls a.txt* 2> /dev/null
unlzop a.txt.lzo
ls a.txt*
cmp a.txt hello.txt && echo "restored"
lzop a.txt.lzo
echo "rc=$?"
lzop a.txt
echo "rc=$?"
lzop -f a.txt && echo "forced"
lzop -d a.txt.lzo
echo "rc=$?"
rm a.txt && lzop -dv a.txt.lzo && cmp a.txt hello.txt && echo "unlzop -v ok"
tar cf t.tar hello.txt && lzop t.tar && mv t.tar.lzo t.tzo && rm t.tar
unlzop t.tzo && ls t.tar
lzop -o out.lzo hello.txt && lzop -dc out.lzo
echo
lzop -d -o back.txt out.lzo && cmp back.txt hello.txt && echo "-o ok"
lzop -o x.lzo hello.txt a.txt
echo "rc=$?"
printf 'from stdin\n' | lzop -o stdin.lzo && lzopcat stdin.lzo
lzop -l hello.txt.lzo out.lzo
lzop -t hello.txt.lzo && echo "test ok"
lzop -tv hello.txt.lzo

echo "=== lzopcat and concatenated files"
printf 'one\n' | lzop -c > 1.lzo
printf 'two\n' | lzop -c > 2.lzo
lzopcat 1.lzo 2.lzo
cat 1.lzo 2.lzo | lzopcat
{ cat 1.lzo; printf 'trailing'; } | lzopcat

echo "=== damage is detected"
lzop -c random > big.lzo
head -c 100000 big.lzo > cut.lzo
lzopcat cut.lzo > /dev/null
echo "rc=$?"
lzop -t cut.lzo
echo "rc=$?"
head -c 30 big.lzo | lzopcat
echo "rc=$?"
cp hello.txt.lzo bad.lzo
printf '\x55' | dd of=bad.lzo bs=1 seek=60 conv=notrunc 2> /dev/null
lzop -t bad.lzo
echo "rc=$?"
lzopcat -F bad.lzo | od -An -c | head -2
lzop -C -c repeat > sums.lzo
printf '\x07' | dd of=sums.lzo bs=1 seek=2000 conv=notrunc 2> /dev/null
lzop -t sums.lzo
echo "rc=$?"
lzop -F -c repeat > bare.lzo
printf '\x07' | dd of=bare.lzo bs=1 seek=2000 conv=notrunc 2> /dev/null
lzop -t bare.lzo
echo "rc=$?"
cp hello.txt.lzo head.lzo
printf '\x00' | dd of=head.lzo bs=1 seek=20 conv=notrunc 2> /dev/null
lzop -t head.lzo
echo "rc=$?"
printf 'plain text' | lzopcat
echo "rc=$?"

echo "=== tar"
mkdir -p src/sub
cp repeat src/sub/repeat
printf 'small\n' > src/small.txt
tar --lzop -cf a.tar.lzo src
od -An -tx1 -N 9 a.tar.lzo
tar tf a.tar.lzo
tar cf b.tar.lzo -a src
lzop -t b.tar.lzo && echo "-a picks lzop"
mkdir out
tar xf b.tar.lzo -C out
cmp out/src/sub/repeat repeat && echo "tar extract ok"
tar --lzop -tf a.tar.lzo src/small.txt
tar -tf - < a.tar.lzo | wc -l

cd / && rm -rf "$tmp"
