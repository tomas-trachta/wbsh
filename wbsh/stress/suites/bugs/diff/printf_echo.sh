#!/bin/bash
# printf formats and echo options.
printf '%s|%5s|%-5s|%.2s\n' a b c defg
printf '%d|%5d|%-5d|%05d|%+d|%x|%X|%o|%#x\n' 42 42 42 42 42 255 255 8 255
printf '%.3f|%10.2f|%e|%g|%g\n' 3.14159 2.5 12345.678 0.0001 1e20
printf '%c|%%|%b\n' hello 'a\tb\n'
printf '%s\n' a b c
printf '%s-%s\n' 1 2 3
printf '[%q]\n' "a b" "it's" ''
printf '%(%Y)T\n' 0 | wc -c
printf '%d\n' "'A" 
echo -n no-newline; echo
echo -e 'a\tb\x41\0101'
echo -E 'a\tb'
echo -- -n
printf -v out '%03d' 7; echo "$out"
printf '%s' ; echo "empty-printf $?"
