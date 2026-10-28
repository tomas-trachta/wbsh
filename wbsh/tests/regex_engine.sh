#!/bin/bash
# The POSIX regex engine behind grep, sed, awk, find -regex, nl, pgrep and
# [[ =~ ]]: long repeats never overflow a stack, matches are leftmost-longest,
# and errors carry GNU's wording.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1

awk 'BEGIN { for (i = 0; i < 300; i++) s = s "a"; print s }' > a300.txt
awk 'BEGIN { for (i = 0; i < 20000; i++) s = s "ab"; print s }' > ab.txt

echo "=== a group repeated hundreds of times still matches"
grep -cE '(a|b)+' a300.txt
echo "rc=$?"
sed -E 's/(a)+/X/' a300.txt
awk '/(a|b)+$/ { print "awk fired" }' a300.txt
[[ $(cat a300.txt) =~ ^(a|b)+$ ]] && echo "=~ matched ${#BASH_REMATCH[0]} chars"

echo "=== a 40000 character line"
grep -cE '^(ab)+$' ab.txt
grep -c '^\(ab\)*$' ab.txt
sed -E 's/(ab)+/X/' ab.txt
awk '{ print match($0, /(ab)+$/), RLENGTH }' ab.txt
grep -c '\(ab\)\1' ab.txt
sed 's/^\(ab\)\(.*\)\1$/<\1>/' ab.txt

echo "=== grep -P keeps std::regex but reports when it gives up"
awk 'BEGIN { for (i = 0; i < 3000; i++) s = s "a"; print s }' | grep -cP '(a|b)+$'
echo "rc=$?"

echo "=== leftmost-longest"
echo foobar | sed -E 's/(foo|foobar)/X/'
echo abcd | sed -E 's/(a|ab)(c|bcd)/[&]/'
[[ abcd =~ (a|ab)(c|bcd) ]] && echo "BASH_REMATCH[0]=${BASH_REMATCH[0]}"
echo foobar | awk '{ print match($0, /fo|foob|foo/), RLENGTH }'
echo 'xabcx' | grep -oE 'a|ab|abc'
printf 'ab abc\n' | grep -o -e ab -e abc

echo "=== empty matches"
echo xyz | sed 's/x*/-/g'
echo abc | sed 's/b*/X/g'
echo abc | sed -E 's/(b|$)/X/g'

echo "=== groups and back-references"
echo 'hello world' | sed 's/\(o\)\(.\)/\2\1/g'
echo 'ab ab' | sed 's/\(ab\) \1/dup/'
echo 'abcabc' | sed -E 's/(abc)\1/dup/'
echo 'abcd' | sed -E 's/(((a)b)c)d/\3\2\1/'
echo 'ab' | sed -E 's/(a)(b)?(c)?/[\1\2\3]/'
printf 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaac\n' | sed -n '/\(a*\)*\1b/p'
echo "rc=$?"
[[ key=value =~ ^([^=]+)=(.*)$ ]] && echo "${BASH_REMATCH[1]}|${BASH_REMATCH[2]}"

echo "=== BRE and ERE operators"
echo 'a+b aab' | sed 's/a+b/X/; s/a\+b/Y/'
echo 'ab' | sed 's/ab\?c\?/X/'
echo '*x' | sed 's/*x/X/'
echo '*a' | sed 's/^*a/X/'
echo 'a^b$c' | sed 's/a^b$c/X/'
echo 'abc' | sed 's/a\|c/X/g'
echo 'aaaa' | sed 's/a\{2,3\}/X/'
echo 'aaa' | grep -o 'a\{,2\}'
echo 'x*a' | grep -oE '*a'
echo 'a)' | grep -E 'a)'
echo 'aaaaa' | grep -oE 'a{1,2}{2}'

echo "=== classes, escapes and anchors"
echo '123abc' | sed 's/[[:digit:]]\+/N/'
echo 'a]b' | sed 's/[]]/X/'
echo 'a-b' | sed 's/[a-]/X/g'
echo 'a1_b c' | sed 's/\w*/W/'
echo 'a b' | sed 's/\s/S/; s/\S/N/'
echo 'aaa bbb' | sed 's/\</</g; s/\>/>/g'
echo 'aaa bbb' | sed 's/\B/-/g'
echo 'abc' | sed 's/\`a/X/; s/c'"\\'"'/Z/'
echo 'd1' | grep -o '\d'
echo '[\n]' | grep -o '[\n]'
printf 'a\nb\n' | sed 'N;s/a.b/X/'
printf 'a\nb\n' | sed 'N;s/^b/X/'
printf 'a\nb\n' | sed 'N;s/^b/X/M'
printf 'a\nb\n' | sed 'N;s/a$/X/M'
printf 'one\nb\n' | sed 'N;/^b/Mp;d'

echo "=== case-insensitive"
echo 'Hello' | sed 's/hello/X/I'
shopt -s nocasematch
[[ ABC =~ ^a.c$ ]] && echo "nocasematch =~"
shopt -u nocasematch
echo 'ÉCOLE' | grep -i 'école'
echo 'école' | grep -io 'É'
echo 'ÉCOLE' | grep -iF 'école'

echo "=== awk dialect"
echo 'a/b' | awk '{ sub(/\//, "|"); print }'
echo 'aXb' | awk '{ sub(/\x58/, "-"); print }'
echo 'aAb' | awk '{ sub(/\101/, "-"); print }'
echo 'a]b' | awk '{ sub(/[\]]/, "R"); print }'
echo 'word here' | awk '{ gsub(/\y/, "|"); print }'
echo 'word here' | awk '{ gsub(/\</, "<"); print }'
echo 'a.b.c' | awk '{ n = split($0, p, /\./); print n, p[3] }'

echo "=== other users"
mkdir -p tree/sub && touch tree/sub/file.txt tree/other.log
find tree -regex '.*/sub/f[a-z]*\.txt'
find tree -regextype posix-extended -regex '.*\.(log|txt)' | sort
find tree -iregex '.*FILE.TXT'
printf 'apple\nbanana\napricot\n' | nl -b 'p^a'

echo "=== invalid patterns"
echo a | grep 'a\{1'
echo "rc=$?"
echo a | grep '\(a'
echo a | grep '[a'
echo a | grep '[[:foo:]]'
echo a | grep '[z-a]'
echo a | grep 'x\'
echo a | grep '\(a\)\2'
echo a | grep 'a\{32768\}'
echo a | sed 's/\(a/b/'
re='('
[[ a =~ $re ]]
echo "=~ rc=$?"

cd / && rm -rf "$D"
