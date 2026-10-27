#!/bin/bash
# xargs: command-line length batching, quote / backslash parsing, -I whole
# lines, -0 / -d / -E, -n / -L / -s / -x / -r / -t / -P / -a, child stdin,
# and GNU exit statuses.

echo "=== batching by length"
seq 1 20000 | xargs echo | wc -lw
printf 'a b c d\n' | xargs -s 12 echo
printf 'a b c d\n' | xargs -s 12 -n 3 -x echo; echo "status $?"
printf 'aaaaaaaaaaaaaaaaaaaa\n' | xargs -s 12 echo; echo "status $?"

echo "=== quotes and backslashes"
printf '"a b" c\n' | xargs -n1 echo
printf "'x  y' z\n" | xargs -n1 echo
printf 'a\\ b c\\\\d\n' | xargs -n1 echo
printf '%s\n' 'a\x41' | xargs echo
printf "it's\n" | xargs echo; echo "status $?"
printf '"abc\n' | xargs echo; echo "status $?"

echo "=== -I takes whole lines"
printf 'a b\nc\n' | xargs -I{} echo "[{}]"
printf '  a b  \n\n c\n' | xargs -I{} echo "[{}]"
printf 'a\nb\n' | xargs -i echo '<{}>'
printf 'a\nb\n' | xargs -iX echo '<X>'
printf 'a\nb\n' | xargs --replace=Q echo 'Q-Q'
printf 'one two\n' | xargs -I % echo % %
printf 'x y\n' | xargs -I{} -n1 echo {}

echo "=== -0 / -d / -E"
printf 'x\0y\0' | xargs -0 echo
printf 'a\nb\n' | xargs --null echo | od -c
printf 'a,b,,c' | xargs -d, echo
printf 'a\nb\n' | xargs -d '\n' -n1 echo
printf 'a\tb\n' | xargs -d '\t' echo
printf 'a b\nSTOP\nc\n' | xargs -E STOP echo
printf 'a b\nSTOP\nc\n' | xargs -eSTOP echo
printf 'a\0b\0' | xargs -0 -I{} echo '({})'

echo "=== -n / -L / -r / empty input"
printf 'a\nb\nc\nd\ne\n' | xargs -n2 echo
printf 'a b\nc\nd e f\n' | xargs -L1 echo
printf 'a b \nc\nd e f\n' | xargs -L1 echo
printf 'a b\nc\nd e f\n' | xargs -L 2 echo
printf '' | xargs echo hi
printf '' | xargs -r echo hi
printf 'a b\n' | xargs

echo "=== -t / -P / -a"
printf 'a\nb\n' | xargs -t echo 2>&1
printf 'a\nb\n' | xargs --max-args=1 --verbose echo 2>&1
seq 1 6 | xargs -P3 -n2 echo | sort
seq 1 4 | xargs -P0 -I{} echo 'n={}' | sort
printf 'one\ntwo\n' > _xargs_items.txt
xargs -a _xargs_items.txt echo from-file
rm -f _xargs_items.txt

echo "=== the command does not get xargs' input"
show_stdin() { cat; echo "arg $1"; }
printf 'a\nb\n' | xargs -n1 show_stdin

echo "=== streaming and a closed output"
yes abc | xargs -n2 echo 2>_xargs_err.txt | head -2
cat _xargs_err.txt
rm -f _xargs_err.txt

echo "=== exit statuses"
fail3() { return 3; }
stop255() { return 255; }
printf 'a\n' | xargs fail3; echo "status $?"
printf 'a\nb\n' | xargs -n1 stop255; echo "status $?"
printf 'a\n' | xargs _xargs_no_such_command; echo "status $?"

echo "=== option errors"
printf 'a\n' | xargs -Q; echo "status $?"
printf 'a b' | xargs -n 0 echo; echo "status $?"
printf 'a\n' | xargs -n x echo; echo "status $?"
printf 'a\n' | xargs -P x echo; echo "status $?"
printf 'a\n' | xargs -L 0 echo; echo "status $?"
printf 'a\n' | xargs -d ab echo; echo "status $?"
