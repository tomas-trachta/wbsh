#!/bin/bash
# sort: -k / -t key fields with per-key options, -s / -u by key, -c / -C,
# -o onto its own input, -m, -z, and the -n -g -h -M -V -R -d -i -f -b
# orderings (GNU coreutils semantics, C locale).

echo "=== keys"
printf 'b 2\na 1\n' | sort -k2,2n
printf 'b 2\na 10\nc 1\n' | sort -k2n
printf 'x:3:b\ny:1:a\nz:2:c\n' | sort -t: -k2,2
printf 'x:3:b\ny:1:a\nz:2:c\n' | sort -t: -k3
printf 'a b c\na a d\nb a a\n' | sort -k2,2 -k3,3r
printf 'a 2\na 10\nb 1\n' | sort -k1,1 -k2n
printf 'foo  bar\nfoo baz\n' | sort -k2
printf 'foo  bar\nfoo baz\n' | sort -k2b
printf 'foo  bar\nfoo baz\n' | sort -b -k2
printf 'aXc\naYb\n' | sort -k1.3
printf 'aXcd\naYbe\n' | sort -k1.2,1.2
printf 'ab cd\nab ce\n' | sort -k1.2,2.1
printf 'x 3\nx 20\nx 100\n' | sort -k2 -n
printf 'a,b\na,a\n' | sort --field-separator=, --key=2
printf 'x:\n:x\n' | sort -t: -k2
printf 'a\tb\na b\n' | sort -t '	' -k2

echo "=== stable / unique by key"
printf '1\n01\n1.0\n' | sort -nu
printf 'a 1\nb 1\nc 0\n' | sort -s -k2,2n
printf 'a 1\nb 1\nc 0\n' | sort -k2,2n
printf 'c 1\nb 1\na 1\n' | sort -k2,2n -u
printf 'B\na\nb\nA\n' | sort -fu

echo "=== orderings"
printf '10K\n1M\n2G\n500\n1k\n-1M\n0\n' | sort -h
printf '1.10\n1.2\n1.9\n1.1a\n1.1\nv2\nv10\n' | sort -V
printf 'a-1.0.tar.gz\na-1.0~rc1.tar.gz\na-1.0.1.tar.gz\n.b\n..\n.\n' | sort -V
printf '1e3\n-5\nabc\n2.5\ninf\n-inf\n0x10\n' | sort -g
printf 'Mar\njan\nFEB\nxyz\nDec\n' | sort -M
printf 'b\nB\na\nA\n_\n' | sort -f
printf 'b\nB\na\nA\n' | sort
printf '  b\n a\nc\n' | sort -b
printf '  b\n a\nc\n' | sort
printf 'a.b\na,c\nab\n' | sort -d
printf 'x\001b\nxa\n' | sort -i | od -c
printf 'a\nb\nc\n' | sort -r
printf '1\n2\n1\n' | sort -nr
printf -- '-1\n-10\n-2\n0\n.5\n-.5\n' | sort -n
printf '1,000\n999\n' | sort -n
printf '3\n20\n100\n' | sort --sort=numeric
printf '3\n20\n100\n' | sort --numeric-sort --reverse
printf 'a\n\nb\n' | sort
printf 'a\nb\na\nc\na\nb\n' | sort -R | uniq | wc -l
printf '3\n1\n2\n' | sort -R | sort -n

echo "=== check / output / merge / -z"
printf 'a\nb\nb\n' | sort -c; echo "status $?"
printf 'a\nb\nb\n' | sort -cu; echo "status $?"
printf 'b\na\n' | sort -c; echo "status $?"
printf 'b\na\n' | sort -C; echo "status $?"
printf 'b\0a\0' | sort -z | od -c
printf 'c\nb\na\n' > _sort_o.txt
sort -o _sort_o.txt _sort_o.txt
cat _sort_o.txt
sort -m _sort_o.txt _sort_o.txt
rm -f _sort_o.txt
printf '5\n3\n' | sort -S 1M -T /tmp --parallel=2

echo "=== errors"
printf 'x\n' | sort -k0; echo "status $?"
printf 'x\n' | sort -k1.0; echo "status $?"
printf 'x\n' | sort -k1x; echo "status $?"
printf 'x\n' | sort -ka; echo "status $?"
printf 'x\n' | sort -t ab; echo "status $?"
printf 'x\n' | sort -ng; echo "status $?"
printf 'x\n' | sort -k1,1nM; echo "status $?"
printf 'x\n' | sort -dn; echo "status $?"
printf 'x\n' | sort -Q; echo "status $?"
printf 'x\n' | sort _sort_missing.txt; echo "status $?"
printf 'x\n' | sort -c a b; echo "status $?"
