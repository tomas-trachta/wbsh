#!/bin/bash
# cut / uniq / nl / rev / paste stream their input (an endless producer
# still yields output), keep NUL bytes, and nl / paste gained their GNU
# options (sections, styles, delimiter escapes).

echo "=== streaming from an endless producer"
yes abc | cut -c1 | head -2
yes abc | nl | head -2
yes abc | rev | head -1
yes abc | paste - - | head -1
yes abc | paste -s | head -c 8 | od -c
yes $'a\nb' | uniq | head -3

echo "=== NUL bytes survive"
printf 'ab\0cd\n' | cut -c1-5 | od -c
printf 'ab\0cd\nab\0cd\n' | uniq -c | od -c
printf 'a\0b\nc' | nl | od -c
printf 'a\0b\n' | rev | od -c
printf 'a\0b\n' | paste - | od -c
printf 'a\0b\nc\n' | tac | od -c

echo "=== rev"
printf 'abc\nxyz' | rev
echo '|'
printf 'h\303\251llo\n' | rev
printf 'ab\0cd\0' | rev -0 | od -c

echo "=== nl styles"
printf 'a\n\nb\n' | nl
printf 'a\n\nb\n' | nl -ba -nln -w3 -s'|'
printf 'a\n\nb\n' | nl -b a -n rz -w 4
printf 'a\n\n\n\nb\n' | nl -ba -l2
printf 'apple\nbanana\ncherry\n' | nl -bp'^[ab]'
printf 'a\nb\n' | nl -v10 -i5
printf 'a\nb\n' | nl --starting-line-number=3 --number-separator=': '

echo "=== nl sections"
printf 'h1\n\\:\\:\\:\nhead\n\\:\\:\nbody\nbody2\n\\:\nfoot\n\\:\\:\\:\nh\n\\:\\:\nb\n' | nl -ha -fa
printf 'x\n\\:\\:\\:\nhead\n\\:\\:\nbody\n' | nl -p
printf 'a\n@:\nb\n' | nl -d@
printf 'a\n\\:\nb\n' | nl -d ''
printf 'a\n' > _ts_f1.txt
printf 'b\n' > _ts_f2.txt
nl _ts_f1.txt _ts_f2.txt
rm -f _ts_f1.txt _ts_f2.txt

echo "=== nl errors"
printf 'a\n' | nl -x; echo "status $?"
printf 'a\n' | nl -bq; echo "status $?"
printf 'a\n' | nl -nxx; echo "status $?"
printf 'a\n' | nl -w0; echo "status $?"
printf 'a\n' | nl -l0; echo "status $?"
printf 'a\n' | nl -vx; echo "status $?"

echo "=== paste"
printf '1\n2\n3\n' > _ts_p1.txt
printf 'a\nb\n' > _ts_p2.txt
: > _ts_empty.txt
paste _ts_p1.txt _ts_p2.txt
paste -d ':,' _ts_p1.txt _ts_p2.txt _ts_p1.txt
paste -s _ts_p1.txt _ts_p2.txt
paste -s -d '\n' _ts_p1.txt
paste -d '\t\\' _ts_p1.txt _ts_p2.txt _ts_p1.txt
paste -d '' _ts_p1.txt _ts_p2.txt
paste -d '\0' _ts_p1.txt _ts_p2.txt
printf '1\n2\n3\n4\n' | paste - -
printf '1\n2\n3\n4\n5\n' | paste - _ts_p2.txt -
printf '1\n2\n3\n' | paste -s -d, -
printf 'a\nb' | paste - _ts_p1.txt
printf 'x\0y\0' | paste -z -s -d, | od -c
paste -s _ts_empty.txt | od -c
paste -s -d, _ts_empty.txt _ts_p1.txt

echo "=== paste errors"
paste -x; echo "status $?"
paste -d 'ab\' _ts_p1.txt; echo "status $?"
paste _ts_missing.txt _ts_p1.txt; echo "status $?"
rm -f _ts_p1.txt _ts_p2.txt _ts_empty.txt
