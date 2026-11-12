#!/bin/bash
# ${x:offset:length} at the edges, as in bash: an offset before the start
# or past the end gives nothing; a negative length counts from the end and
# must not end before the offset (an error that drops the rest of the
# line); an unset variable gives nothing without these checks.

x=abc e=
a=(xy)
for spec in ' -2' ' -3' ' -4' '5' '3' '2:-1' '1:-2' ' -4:1' '0:-3' '4:-1' ' -1:-1' \
        ' -2:-1' '1' '1:1' '0:10' ' -1:5'; do
    eval 'echo "x:$spec -> [${x:'"$spec"'}]"'
done
echo "unset -> [${u:0:-1}]"
echo "unset element -> [${a[1]:0:-1}]"
for spec in '1:-5' '3:-1' '0:-4' '1:2-7'; do
    eval 'echo "${x:'"$spec"'}"; echo not reached'
    echo "x:$spec status $?"
done
eval 'echo "${e:0:-1}"; echo not reached'
echo "empty status $?"
y=ěščř
echo "utf-8: [${y: -2}] [${y:1:-1}] [${y: -5}]"
