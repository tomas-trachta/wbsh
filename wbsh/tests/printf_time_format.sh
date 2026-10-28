#!/bin/bash
# printf %(datefmt)T: epoch seconds formatted with strftime.

echo "=== basic"
printf '[%(%Y-%m)T]\n' 1700000000
printf '[%(%s)T]\n' 1700000000 5
printf '%(%Y)T\n' 1700000000 1700000000
printf '[%10(%Y)T][%-6(%Y)T][%.2(%Y)T]\n' 1700000000 1700000000 1700000000

echo "=== now and shell start"
now=$(date +%s)
t=$(printf '%(%s)T' -1)
(( t - now <= 2 && t >= now )) && echo "-1 is now"
t=$(printf '%(%s)T')
(( t - now <= 2 && t >= now )) && echo "no argument is now"
t=$(printf '%(%s)T' -2)
(( t <= now && now - t < 600 )) && echo "-2 is the shell start"

echo "=== printf -v"
printf -v year '%(%Y)T' 1700000000
echo "year=$year"

echo "=== default format is %X"
[ "$(printf '%()T' 1700000000)" = "$(printf '%(%X)T' 1700000000)" ] && echo "empty format is %X"

echo "=== GNU extensions"
printf '[%(%-m|%_m|%k|%l|%P)T]\n' 1700000000 | sed 's/[0-9]/N/g'
printf '[%(%%|%n|%^a)T]\n' 1700000000 | tr '\n' '~' | sed 's/~$//'; echo

echo "=== errors"
printf '[%(%Y)T]\n' abc
echo "status $?"
printf '[%(%Y\n' 1
echo "status $?"
