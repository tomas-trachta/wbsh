#!/bin/bash
# <(cmd) and >(cmd) are part of the word they are in, as in bash: the
# word keeps its other text, and x=<(cmd) is an assignment, not `x=`
# followed by a command named after the pipe.

set -- a<(true)b; echo "1 words: $#, starts a: ${1:0:1}, ends b: ${1: -1}"
x=<(exit 3); echo "2 status $?, set: ${x:+yes}"
echo pre<(true) | grep -c "^pre." | sed "s/^/3 /"
echo --file=<(true) | grep -c '^--file=.' | sed 's/^/4 /'
declare z=<(true); echo "5 declared: ${z:+yes}"
arr=(<(true) b); echo "6 elements: ${#arr[@]}"
cat <(echo seven)
paste <(echo s8a) <(echo s8b)
echo "9 <(quoted)" '<(single)'
wc -l < <(printf 'a\nb\n') | sed 's/^/10 /'
set -- 2>(true); echo "11 words: $#, starts 2: ${1:0:1}"
diff <(echo a) <(echo a) && echo 12 same
<(true) 2>/dev/null; echo "13 a pipe path as a command: $?"
<(true) 2>/dev/null | cat; echo "14 ${PIPESTATUS[*]}"
