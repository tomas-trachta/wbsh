#!/bin/bash
# hexdump -e / -f format strings with util-linux semantics: iteration and
# byte counts, every conversion, blank padding of a short final block, the
# dropped trailing blank of a unit's last round, %_A, squeezing, -s / -n,
# and the canned formats written out as -e strings. Line ends are marked
# with | so trailing blanks show.

D=_hdf_tmp
rm -rf $D 2>/dev/null
mkdir -p $D
cd $D || exit 1
printf 'hello world, this is hexdump!\n' > h.txt
marked() { sed 's/$/|/'; }

echo "=== bytes per line"
hexdump -e '16/1 "%02x " "\n"' h.txt | marked

echo "=== address and words"
hexdump -e '"%07.7_ax " 8/2 "%04x " "\n"' h.txt | marked

echo "=== one word per line"
printf 'abcdefghij' | hexdump -e '1/4 "%08x\n"' | marked

echo "=== default format spelled out"
hexdump -e '"%07.7_Ax\n"' -e '"%07.7_ax " 8/2 "%04x " "\n"' h.txt | marked
printf '' | hexdump -e '"%07.7_Ax\n"' -e '"%07.7_ax " 8/2 "%04x " "\n"' | wc -c

echo "=== canonical spelled out"
hexdump -e '"%08.8_ax  " 8/1 "%02x " "  " 8/1 "%02x "' -e '"  |" 16/1 "%_p" "|\n"' \
    -e '"%08.8_Ax\n"' h.txt > spelled.out
hexdump -C h.txt | cmp - spelled.out && echo "same as -C"
cat spelled.out

echo "=== character conversions"
printf 'a\tb\000\177\200 \n~' > chars.bin
hexdump -e '8/1 "%4_c" "\n"' chars.bin | marked
hexdump -e '8/1 "%4_u" "\n"' chars.bin | marked
hexdump -e '8/1 "%_p" "\n"' chars.bin | marked
printf 'xyz' | hexdump -e '3/1 "<%c>" "\n"'

echo "=== integers"
printf '\377\377\376\377\001\002\003\004' > ints.bin
hexdump -e '4/2 "%6d" "\n"' ints.bin
hexdump -e '2/4 "%u " "\n"' ints.bin | marked
hexdump -e '8/1 "%i " "\n"' ints.bin | marked
hexdump -e '8/1 "%03o " "\n"' ints.bin | marked
hexdump -e '1/8 "%016X\n"' ints.bin
hexdump -e '2 "%x " "\n"' ints.bin | marked
hexdump -e '"%-6x|" "%+5d|" "%#o\n"' ints.bin

echo "=== floats"
printf '\000\000\200\077\000\000\000\000\000\000\370\077' > floats.bin
hexdump -n 4 -e '1/4 "%f\n"' floats.bin
hexdump -s 4 -e '1/8 "%g "' -e '1/8 "%e "' -e '1/8 "%E "' -e '1/8 "%G\n"' floats.bin
hexdump -s 4 -e '"%.2f\n"' floats.bin

echo "=== strings"
printf 'abcdefgh' | hexdump -e '2/4 "[%s]" "\n"'
printf 'abcdefgh' | hexdump -e '"%.3s|"'; echo
printf 'ab\000defgh' | hexdump -e '1/8 "[%-10s]\n"'

echo "=== percent and escapes"
printf 'AB' | hexdump -e '1/1 "%%%02x%%\t" "\n"' | marked
printf 'AB' | hexdump -e '"\\" 2/1 "\a%_c" "\n"' | od -An -c

echo "=== skip and length"
hexdump -s 6 -n 5 -e '"%_ad: " 5/1 "%c" "\n"' h.txt
hexdump -s 6 -n 5 -e '"%_ao/%_ax: " 5/1 "%c" "\n"' -e '"end %_Ad\n"' h.txt

echo "=== short final block"
printf 'abc' | hexdump -e '"%07.7_Ax\n"' -e '"%07.7_ax " 8/2 "%04x " "\n"' | marked
printf 'abcde' | hexdump -e '"%_ad " 4/1 "%02x," "\n"' | marked

echo "=== last unit fills the block"
printf '\001\0\0\0\002\0\0\0\003\0\0\0\004\0\0\0' > words.bin
{ hexdump -e '16/1 "%02x" "\n"' -e '"%x "' words.bin; echo; } | marked

echo "=== squeezing"
head -c 64 /dev/zero > z.bin
printf 'tail' >> z.bin
hexdump -e '"%07.7_ax " 16/1 "%02x" "\n"' -e '"%07.7_Ax\n"' z.bin | marked
hexdump -v -e '"%07.7_ax " 16/1 "%02x" "\n"' z.bin | wc -l

echo "=== format file"
cat > fmt.txt <<'EOF'
# offset, then the bytes by name

    "%06.6_ao "  12/1 "%3_u"
"\n"
EOF
hexdump -f fmt.txt h.txt | marked
hexdump -f fmt.txt -e '"%_Ax\n"' h.txt | tail -n 1

echo "=== canned flags mixed with -e"
hexdump -C -e '"%_Ad\n"' h.txt
hd -n 4 -e '"%_Ax\n"' h.txt

echo "=== errors"
hexdump -e '1/3 "%x"' h.txt; echo "status $?"
hexdump -e '"%s"' h.txt; echo "status $?"
hexdump -e '2/1 "%x %x"' h.txt; echo "status $?"
hexdump -e '"%y"' h.txt; echo "status $?"
hexdump -e '"%_z"' h.txt; echo "status $?"
hexdump -e '"%_aq"' h.txt; echo "status $?"
hexdump -e '16/1 %x' h.txt; echo "status $?"
hexdump -e '"%x' h.txt; echo "status $?"
hexdump -e '4/2 "%c"' h.txt; echo "status $?"
hexdump -f missing.fmt h.txt; echo "status $?"

cd ..
rm -rf $D
echo "=== done"
