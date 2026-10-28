#!/bin/bash
# ed driven through stdin: addresses, every command, substitution,
# global commands, undo, files and shell escapes, errors and exit status.

tmp=$(mktemp -d)
cd "$tmp" || exit 1

sample() {
    printf 'one\ntwo\nthree\nfour\nfive\n' > f.txt
}

# run COMMANDS...: one command per argument, fed to ed -s on f.txt, then Q.
run() {
    printf '%s\n' "$@" Q | ed -s f.txt
    echo "rc=$?"
}

echo "=== loading, printing and byte counts"
sample
printf '%s\n' ',p' 'q' | ed f.txt
printf '%s\n' 'q' | ed missing.txt
echo "rc=$?"
sample; run ',n' '2,3l' '$=' '.=' '=' '3' '-' '+2'
sample; run '2' '' '' 'p'
sample; run 'z2' '1z2' 'z' '1z1' 'z'
printf 'tab\there\\back\001\n' > l.txt
printf '%s\n' 'l' 'q' | ed -s l.txt
awk 'BEGIN { for (i = 0; i < 100; i++) printf "%d", i % 10; print "" }' > long.txt
printf '%s\n' 'l' 'pn' 'q' | ed -s long.txt

echo "=== addresses"
sample; run '/t/p' '//p' '??p' '?o?p' '/^f/,$p' "2ka" "'a,'a+1p" '1;+2p' '%p'
sample; run '$-2,$p' '-,.p' '^p' '2 1p' ',p' ';p'
sample; run '/nothing/p' '9p' "'zp" '0p' '3,1p'

echo "=== appending, inserting, changing, deleting"
sample; run 'a' 'after one?' '.' '0a' 'at top' '.' '$a' 'at end' '.' ',p'
sample; run '3i' 'before three' '.' '0i' 'first' '.' ',n'
sample; run '2,3c' 'changed' '.' ',p' '.='
sample; run '2,4d' ',p' '.=' '$d' ',p' '.='
sample; run ',d' '.=' 'a' 'fresh' '.' ',p'

echo "=== join, move, transfer, marks"
sample; run '1,3j' ',p' '2j' '$-1,$jp'
sample; run '1m$' ',p' '$m0' ',p' '2,3m4' ',p'
sample; run '2,3m2' '1,3m2' 'h'
sample; run '1,2t$' ',p' '5t0' ',n'
sample; run '3kx' '1,2d' "'xp" "'x,\$p" '1kq' 'u' "'qp"

echo "=== substitute"
sample; run 's/o/0/' 'p' '2s/w/W/p' ',s/e/E/g' ',p'
sample; run ',s/o/[&]/gp' 'u' ',s/\(.\)\(.*\)/\2\1/' ',p'
sample; run '1s/one/a\' 'b/' ',p'
sample; run 's/x/y/' 'H' 's/o/0' ',s/e/E/2' ',p'
sample; run '3s/e/X/' '3s' 'u' '3s//Y/' '5s/f/%/' ',p'
sample; run '1s/o/A/' '2s' '4sg' ',p'
sample; run '1s|n|/|' 's#o#\##' 'p' 's/[/]/x/' 'p'
sample; run ',s/^/> /' ',s/$/ </' ',p'
sample; run ',s/[aeiou]*/-/g' ',p'

echo "=== global commands"
sample; run 'g/o/p' 'g/e/s//E/' ',p'
sample; run 'v/e/d' ',p'
sample; run 'g/^/m0' ',p'
sample; run 'g/o/s/o/0/\' 's/0/(&)/' ',p'
sample; run 'g/t/a\' 'added' ',p'
sample; run 'g/four/t0\' '1s/four/FOUR/' ',p' 'u' ',p'
sample; run 'g/t/g/o/p' 'g/zz/p' 'g//p'
sample
printf '%s\n' 'G/o/' 'p' '&' '' 'Q' | ed -s f.txt
sample
printf '%s\n' 'V/o/' 's/$/!/' 'Q' | ed -s f.txt

echo "=== undo"
sample; run '2d' 'u' ',p' 'u' ',p'
sample; run 'u' 'h'
sample; run ',s/o/0/g' 'u' 'u' 'u' ',p'
sample; run 'g/e/d' 'u' ',p'

echo "=== files"
sample; run 'w out.txt' 'f' '1,2w part.txt' '3W part.txt' 'f other.txt' 'f' 'q'
cat out.txt part.txt
sample; run '2r part.txt' ',p' '0r part.txt' '1,3p' 'r' '.='
sample; run 'e part.txt' ',p' 'E f.txt' ',p' 'q'
sample; run 'd' 'e part.txt' 'e part.txt' ',p' 'q'
sample; run 'd' 'q' ',p'
sample; run 'd' 'Q'
sample; run 'wq'
cat f.txt
printf '%s\n' 'a' 'new' '.' 'w' 'q' | ed -s
echo "rc=$?"
printf '%s\n' 'a' 'new' '.' 'w named.txt' 'f' 'w' 'q' | ed -s
cat named.txt
printf 'a\r\nb\r\n' > dos.txt
printf '%s\n' '$a' 'c' '.' ',s/$/!/' 'w' 'q' | ed dos.txt
od -c dos.txt
printf 'no newline' > noeol.txt
printf '%s\n' ',p' 'w' 'q' | ed noeol.txt
od -c noeol.txt

echo "=== shell commands"
sample; run '!echo hello' '1r !echo from shell' '1,2p' 'w !tr a-z A-Z' 'Q'
printf '%s\n' '!echo %' 'f' 'Q' | ed -s f.txt
printf '%s\n' '!echo again' '!!' 'Q' | ed f.txt
printf '%s\n' ',p' 'Q' | ed -s '!printf "x\ny\n"'
printf '%s\n' 'e !echo replaced' ',p' 'f' 'Q' | ed -s f.txt

echo "=== prompt, help and errors"
sample
printf '%s\n' 'p' 'P' 'p' 'P' 'p' 'q' | ed -s -p '> ' f.txt
echo
printf '%s\n' 'P' 'p' 'q' | ed -s f.txt
echo
sample; run 'k' 'kA' 'x' 'pz' '1,2,3p' 'w' 'e' 'h' 'H' 'zz' 'q'
printf '%s\n' '9p' 'q' | ed -v -s f.txt
echo "rc=$?"
printf '%s\n' '9p' 'q' | ed -l -s f.txt
echo "rc=$?"
printf '9p\n2p\nq\n' > script.ed
ed -s f.txt < script.ed
echo "rc=$?"
ed -x f.txt < /dev/null
echo "rc=$?"
ed a b < /dev/null
echo "rc=$?"

cd / && rm -rf "$tmp"
