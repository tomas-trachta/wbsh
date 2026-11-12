#!/bin/bash
# An arithmetic error in an array subscript or a slice offset fails the
# command and drops the rest of the input line, as in bash. Correct
# subscripts still work.

n='p 2'
a=(x y z)
echo "1 [${a[n]}]"; echo 1 not reached
a[n]=1; echo 2 not reached
echo "3 [${a[@]:n}]"; echo 3 not reached
unset 'a[n]'; echo 4 not reached
echo "5 [${#a[n]}]"; echo 5 not reached
b=([n]=1); echo 6 not reached
declare c; declare 'c[n]=1'; echo 7 not reached
echo "8 a is still: ${a[*]}"
i=1
echo "9 ${a[i]} ${a[i+1]} ${a[-1]} ${a[@]:1:1}"
a[i*2]=Z; unset 'a[0]'
echo "10 ${a[*]} / ${!a[*]}"
