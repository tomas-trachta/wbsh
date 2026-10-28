#!/bin/bash
# POSIX / GNU regex semantics: an ERE { that opens no interval is a literal,
# alternation is leftmost-longest, and . and brackets match UTF-8 characters.

echo "=== ERE { without an interval is literal"
echo 'a{b' | grep -E 'a{b'
echo 'a{2' | grep -oE 'a{2'
echo 'a{2,3' | grep -oE 'a{2,3'
echo 'a{x}' | grep -oE 'a{x}'
echo 'a{ 2}' | grep -oE 'a{ 2}'
echo 'a{-1}' | grep -oE 'a{-1}'
echo 'ab{' | grep -oE 'a{1,2}b{'
echo '{x' | grep -oE '^{'
echo '{1}a' | grep -oE '{1}a'
echo 'a{2' | sed -E 's/a{2/X/'
echo 'a{b' | awk '/a{b/ { print "awk literal" }'
[[ 'a{b' =~ a{b ]] && echo "=~ literal"
echo "=== real intervals still work"
echo 'aaaa' | grep -oE 'a{2}'
echo 'aaa' | grep -oE 'a{,2}'
echo 'aaa' | grep -oE 'a{2,}'
echo 'aaa' | grep -oE 'a{,}'
echo 'aaaaa' | grep -oE 'a{1,2}{2}'
echo "=== malformed intervals are errors"
echo aa | grep -E 'a{}'
echo "rc=$?"
echo aa | grep -E 'a{2,1}'
echo aa | grep -E 'a{99999}'
echo aa | grep 'a\{2'

echo "=== leftmost-longest alternation"
echo foobar | sed -E 's/(foo|foobar)/X/'
echo foobar | sed 's/\(foo\|foobar\)/X/'
echo 'xabcx' | grep -oE 'a|ab|abc'
echo abcd | sed -E 's/(a|ab)(c|bcd)/[&]/'
[[ abcd =~ (a|ab)(c|bcd) ]] && echo "BASH_REMATCH[0]=${BASH_REMATCH[0]}"
echo foobar | awk '{ print match($0, /fo|foob|foo/), RLENGTH }'
echo 'abcabc' | awk '{ n = gsub(/a|ab|abc/, "<&>"); print n, $0 }'
echo 'int integer' | grep -oE 'in|int|integer'
echo 'x=10' | sed -E 's/[a-z]|[a-z]=[0-9]+/<&>/'

echo "=== . and brackets match whole UTF-8 characters"
echo 'αβγ' | sed 's/./X/'
echo 'αβγ' | sed 's/./&|/g'
echo 'αβγ' | grep -o . | wc -l
echo 'aéb' | grep -c '^a.b$'
echo 'aéb' | grep -o '[^ab]'
echo 'αβγδ' | sed 's/[β-γ]/-/g'
echo 'ñandú x' | grep -o '[[:alpha:]]*'
echo 'čeština' | sed 's/\w\+/W/'
echo 'Ünïcödé' | sed 's/[[:upper:]]/U/g'
echo 'naïve' | awk '{ sub(/.ve/, "VE"); print }'
[[ 'žluť' =~ ^(.)(.*)(.)$ ]] && echo "${BASH_REMATCH[1]} ${BASH_REMATCH[2]} ${BASH_REMATCH[3]}"
echo '日本語' | sed -E 's/(.)(.)/\2\1/'
echo 'x€y' | grep -o 'x.y'
echo 'aé' | grep -c '^a[[=é=]]$'
