#!/bin/bash
# vi driven through stdin: when stdin is not a console the keys are read
# from it as bytes (ESC is Escape, CR or LF is Enter), nothing is drawn,
# and status messages go to stderr one per line.

tmp=$(mktemp -d)
cd "$tmp" || exit 1

sample() {
    printf 'alpha beta gamma\n  indented (paren) line\n\nlast line here.\n' > s.txt
}

# keys KEYS [VI ARGS...]: run vi on s.txt with KEYS, then show the file.
keys() {
    local input=$1
    shift
    printf "$input" | vi "$@" s.txt
    echo "rc=$?"
    cat s.txt
}

echo "=== inserting and writing"
printf 'ihello world\033:wq\n' | vi new.txt
echo "rc=$?"
cat new.txt
printf 'ione\ntwo\033Othree\033:x\n' | vi lines.txt
cat lines.txt
printf ':q\n' | vi lines.txt
echo "rc=$?"
printf 'ihello\033' | vi unsaved.txt
echo "rc=$?"
ls unsaved.txt 2> /dev/null || echo "not written"

echo "=== motions"
sample; keys 'wx$xjx0x:wq\n'
sample; keys 'jwwD:wq\n'
sample; keys 'eaE\033bbiB\033:wq\n'
sample; keys 'W3lx:wq\n'
sample; keys 'jfax;x,x:wq\n'
sample; keys 'jtpx2T(x:wq\n'
sample; keys 'j%%x:wq\n'
sample; keys '}x:wq\n'
sample; keys 'G{dd:wq\n'
sample; keys 'jj+x-x:wq\n'
sample; keys '3Gx1Gxggx:wq\n'
sample; keys 'j10|x:wq\n'
sample; keys 'G^x:wq\n'
sample; keys 'EaX\033BiY\033:wq\n'
printf 'One two. Three four!  Five six?\nSeven (eight.)\n\nNine ten.\n' > sent.txt
printf ')x)x)x)x)x:wq\n' | vi sent.txt
cat sent.txt
printf 'One two. Three four!  Five six?\nSeven (eight.)\n\nNine ten.\n' > sent.txt
printf 'G$(x(x(x(x:wq\n' | vi sent.txt
cat sent.txt

echo "=== operators"
sample; keys 'dw:wq\n'
sample; keys 'd2w:wq\n'
sample; keys '2dw:wq\n'
sample; keys 'wdwjdw:wq\n'
sample; keys 'jjdw:wq\n'
sample; keys 'wcwXX\033:wq\n'
sample; keys 'ecwX\033:wq\n'
sample; keys 'wc$END\033:wq\n'
sample; keys 'dj:wq\n'
sample; keys 'jdk:wq\n'
sample; keys 'jd}:wq\n'
sample; keys 'wd/line\n:wq\n'
sample; keys 'yyjp:wq\n'
sample; keys 'jywP:wq\n'
sample; keys 'y$$p:wq\n'
sample; keys '>>j>j:wq\n'
sample; keys ':set sw=2 et\n>>j<<:wq\n'
sample; keys '3dd:wq\n'
sample; keys '9dd:wq\n'
sample; keys 'cc\033:wq\n'
sample; keys ':set ai\njcctext\033:wq\n'

echo "=== single-key edits"
sample; keys '3x:wq\n'
sample; keys '$X:wq\n'
sample; keys 'wD:wq\n'
sample; keys 'wCnew\033:wq\n'
sample; keys 'Yjp:wq\n'
sample; keys 'sS\033:wq\n'
sample; keys 'jSnew\033:wq\n'
sample; keys 'J:wq\n'
sample; keys '3J:wq\n'
sample; keys 'gJ:wq\n'
sample; keys '6~:wq\n'
sample; keys 'r_3lr-:wq\n'
sample; keys 'wr\n:wq\n'
sample; keys 'Rnew\033:wq\n'
sample; keys 'Rabcd\010\010\033:wq\n'
sample; keys 'xp:wq\n'
sample; keys 'ddp:wq\n'
sample; keys 'ddP:wq\n'
sample; keys 'yy3p:wq\n'
sample; keys 'yw$p:wq\n'

echo "=== insert mode"
sample; keys 'aX\033:wq\n'
sample; keys 'AX\033:wq\n'
sample; keys 'jIX\033:wq\n'
sample; keys 'jgIX\033:wq\n'
sample; keys 'oX\033OY\033:wq\n'
sample; keys ':set ai\njoX\033:wq\n'
sample; keys ':set ai\njo\033:wq\n'
sample; keys ':set ai\njA\nmore\033:wq\n'
sample; keys '3iab\033:wq\n'
sample; keys '2oxy\033:wq\n'
sample; keys 'A one two\027three\033:wq\n'
sample; keys 'A typed\025\033:wq\n'
sample; keys 'iab\010c\033:wq\n'
sample; keys 'jI\010\033:wq\n'
sample; keys 'i\026\001\033:wq\n'
sample; keys 'A\tx\033:wq\n'
sample; keys ':set et ts=4\nA\tx\033:wq\n'
sample; keys ':set sw=4\ni\024\033:wq\n'
sample; keys 'ywo\022"\033:wq\n'

echo "=== counts, undo, redo and repeat"
sample; keys 'dwj.j.:wq\n'
sample; keys 'xxxuu:wq\n'
sample; keys 'xxxuu\022:wq\n'
sample; keys 'ddjddu:wq\n'
sample; keys 'Ahi\033j.:wq\n'
sample; keys 'cwnew\033w.:wq\n'
sample; keys 'x3.:wq\n'
sample; keys '2x.:wq\n'
sample; keys 'u:wq\n'
sample; keys 'ddu:q\n'

echo "=== registers and marks"
sample; keys '"ayyj"Ayy"ap:wq\n'
sample; keys '"qdwj"qP:wq\n'
sample; keys 'ddjdd"1p"2p:wq\n'
sample; keys 'yyjdd"0p:wq\n'
sample; keys 'jmaGd'"'"'a:wq\n'
sample; keys 'wmbG`bx:wq\n'
sample; keys 'Gx'"''"'x:wq\n'
sample; keys '"_ddp:wq\n'

echo "=== searching"
sample; keys '/line\nx:wq\n'
sample; keys '/line\nnx:wq\n'
sample; keys 'G?a\nnx:wq\n'
sample; keys '/l.ne\nNx:wq\n'
sample; keys '*x:wq\n'
sample; keys 'G#x:wq\n'
sample; keys '/nothing\n:wq\n'
sample; keys ':set ic\n/ALPHA\nx:wq\n'
sample; keys ':set nows\nG/alpha\n:wq\n'
sample; keys '/\\<l\nD:wq\n'

echo "=== ex commands"
sample; keys ':2\nx:wq\n'
sample; keys ':$\nx:wq\n'
sample; keys ':2d\n:wq\n'
sample; keys ':1,2d\n:wq\n'
sample; keys ':%%d\n:wq\n'
sample; keys ':2,3y\nG:pu\n:wq\n'
sample; keys ':1m$\n:wq\n'
sample; keys ':$m0\n:wq\n'
sample; keys ':1t.\n:1co$\n:wq\n'
sample; keys ':1,2j\n:wq\n'
sample; keys ':2>\n:wq\n'
sample; keys ':.,+1>>\n:wq\n'
sample; keys ':/last/d\n:wq\n'
sample; keys ':2;+1d\n:wq\n'
sample; keys 'jmx:'"'"'x,$d\n:wq\n'
sample; keys ':2ka\n:'"'"'ad\n:wq\n'
sample; keys ':=\n:2p\n:%%p\n:q\n'
sample; keys ':nosuch\n:wq\n'
sample; keys ':9d\n:wq\n'

echo "=== substitute and global"
sample; keys ':s/a/A/\n:wq\n'
sample; keys ':s/a/A/g\n:wq\n'
sample; keys ':%%s/e/E/g\n:wq\n'
sample; keys ':2,3s/^/> /\n:wq\n'
sample; keys ':%%s/\\(a\\)\\(l\\)/\\2\\1/\n:wq\n'
sample; keys ':s/beta/[&]/\n:s/gamma/\\U&\\E!/\n:wq\n'
sample; keys ':s/ /\\r/g\n:wq\n'
sample; keys ':s/alpha/\\u&/\n:2s/line/~~/\n:wq\n'
sample; keys ':s/a/1/\n:&&\n:s\nj&:wq\n'
sample; keys ':s/A/x/i\n:wq\n'
sample; keys ':%%s/nothing/x/\n:%%s/nothing/x/e\n:wq\n'
sample; keys ':%%s/e/E/gn\n:wq\n'
sample; keys ':%%s#a#/#g\n:wq\n'
sample; keys '/gam\n:s//GAM/\n:wq\n'
sample; keys ':g/line/d\n:wq\n'
sample; keys ':v/line/d\n:wq\n'
sample; keys ':g!/a/s/^/#/\n:wq\n'
sample; keys ':g/a/s/a/@/g\n:wq\n'
sample; keys ':g/^/m0\n:wq\n'
sample; keys ':g/e/p\n:q\n'
sample; keys ':g/a/j\n:wq\n'

echo "=== set"
sample; keys ':set ts=4 sw=2 ai nu list\n:set\n:set ts?\n:set noai\n:set ai?\n:set invnu\n:set nu?\n:q\n'
sample; keys ':set all\n:q\n'
sample; keys ':set bogus\n:set ts=0\n:q\n'

echo "=== files"
sample
printf 'other\n' > o.txt
keys ':r o.txt\n:wq\n'
sample; keys ':0r o.txt\n:wq\n'
sample; keys ':r !echo shell\n:wq\n'
sample; keys ':$r !printf "a\\nb\\n"\n:wq\n'
sample; keys ':!echo run\n:q\n'
sample; keys ':2,3!sort\n:wq\n'
sample; keys ':w !wc -l\n:q\n'
sample; keys ':w copy.txt\n:q\n'
cat copy.txt
sample; keys ':w copy.txt\n:w! copy.txt\n:q\n'
sample; keys ':1w >> o.txt\n:q\n'
cat o.txt
sample; keys 'x:e o.txt\n:e! o.txt\n:wq\n'
cat o.txt
sample; keys 'x:e!\n:wq\n'
sample; keys ':f\n:f renamed.txt\n:w\n:q\n'
cat renamed.txt
sample; keys '\007jj\007:q\n'
mkdir sub
printf ':e sub\n:q\n' | vi
echo "rc=$?"

echo "=== quitting"
sample; keys 'xZZ'
sample; keys 'xZQ'
sample; keys 'x:q\n:q!\n'
sample; keys ':x\n'
sample; keys 'x:wq!\n'

echo "=== command line options"
sample; keys 'x:wq\n' +3
sample; keys 'x:wq\n' +
sample; keys 'x:wq\n' +/indented
sample; keys ':wq\n' -c '%s/a/A/g'
sample; keys 'x:wq\n:q!\n' -R
sample; printf 'x:w\n:w!\n:q\n' | view s.txt; cat s.txt
printf 'x\n' | vi -Z s.txt
echo "rc=$?"

echo "=== several files"
printf 'one\n' > m1.txt
printf 'two\n' > m2.txt
printf 'three\n' > m3.txt
printf ':args\nx:n\n:w\n:n\nx:wn\n:args\n:n\n:prev\n:rew\n:q\n:q\n' | vi m1.txt m2.txt m3.txt
echo "rc=$?"
cat m1.txt m2.txt m3.txt
printf 'x:n!\nAY\033:x\n' | vi m1.txt m2.txt
cat m1.txt m2.txt
printf ':N\n:q\n' | vi m1.txt
printf ':qa\n' | vi m1.txt m2.txt
echo "rc=$?"

echo "=== line endings"
printf 'a\r\nb\r\n' > dos.txt
printf 'ox\033:wq\n' | vi dos.txt
od -c dos.txt
printf 'a\r\nb\n' > mixed.txt
printf ':wq\n' | vi mixed.txt
od -c mixed.txt
printf 'no newline' > noeol.txt
printf 'A!\033:wq\n' | vi noeol.txt
od -c noeol.txt
printf 'a\n' > ff.txt
printf ':set ff=dos\n:wq\n' | vi ff.txt
od -c ff.txt
: > empty.txt
printf ':wq\n' | vi empty.txt
wc -c < empty.txt
printf 'dd:wq\n' | vi lines.txt
wc -c < lines.txt

echo "=== UTF-8, tabs and long lines"
printf 'čeština 日本語 ok\n' > u.txt
printf 'xwxwx:wq\n' | vi u.txt
cat u.txt
printf 'žluťoučký kůň\n' > u.txt
printf '$x0lr*3~:wq\n' | vi u.txt
cat u.txt
printf '日本語\n' > u.txt
printf 'lylp:wq\n' | vi u.txt
cat u.txt
printf 'x\200y\n' > raw.txt
printf 'lx:wq\n' | vi raw.txt
od -c raw.txt
printf 'ab\tcd\n' > tab.txt
printf '$i|\033:wq\n' | vi tab.txt
od -c tab.txt
printf '\t\tx\n' > tab.txt
printf 'j0x:wq\n' | vi tab.txt
od -c tab.txt
awk 'BEGIN { for (i = 0; i < 300; i++) printf "w%d ", i; print "" }' > long.txt
printf '100wD:wq\n' | vi long.txt
wc -w < long.txt

echo "=== scrolling"
seq 1 100 > n.txt
printf '\006\007\006\007\002\007:q\n' | vi n.txt
printf '\004\007\004\007\025\007:q\n' | vi n.txt
printf '\005\005\007\031\007:q\n' | vi n.txt
printf 'H\007M\007L\007G\007H\007M\007L\0073H\0073L\007:q\n' | vi n.txt
printf '50%%\007%%x:q!\n' | vi n.txt
printf '\006\006\006\006\006\006x\007:q!\n' | vi n.txt
printf '\002x:q!\n' | vi n.txt

cd / && rm -rf "$tmp"
