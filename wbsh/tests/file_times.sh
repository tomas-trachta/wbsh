#!/bin/bash
# touch -d / -t / -r / -a / -m / -c, stat -c / --printf / -t, ls --full-time.
# Local times are compared without the zone offset so the output does not
# depend on the machine's time zone.

D=$(mktemp -d) || exit 1
cd "$D" || exit 1
local_time() { stat -c %y "$1" | cut -c1-29; }

echo "=== touch -d"
touch -d '2023-11-14 22:13:20' a; local_time a
touch -d '2023-11-14T22:13:20.5Z' b; stat -c '%Y %.3Y' b
touch -d '@1700000000' c; stat -c %Y c
touch -d '2023-11-14 12:00 +02:00' f; stat -c %Y f
touch -d '2023-11-14' g; local_time g
touch -d '2023-11-14 22:13:20 2 days ago' i; local_time i
touch -d '2023-11-14 22:13:20 3 hours' j; local_time j
touch -d 'garbage x' z; echo "status $?"
[ -e z ] || echo "no file for a bad date"

echo "=== touch -t"
touch -t 202301021304.05 d; local_time d
touch -t 2301021304 e; local_time e
touch -t 13 bad; echo "status $?"

echo "=== touch -r, -a, -m, -c"
touch -r a h; [ "$(stat -c %Y h)" = "$(stat -c %Y a)" ] && echo "-r copies the time"
touch -r a -d '1 day' k; echo "$(( $(stat -c %Y k) - $(stat -c %Y a) ))"
touch -a -d '@1600000000' a; stat -c '%X %Y' a | sed "s/$(stat -c %Y a)/MTIME/"
touch -m -d '@1650000000' a; stat -c '%X %Y' a
touch -c nothere; [ -e nothere ] || echo "-c creates nothing"
touch new; [ -f new ] && echo "plain touch creates"

echo "=== stat -c"
printf 'hello\n' > s
stat -c '%n|%s|%F|%a|%A|%b|%B|%h' s
stat -c '%N' s
stat --printf='%n\t%s\n' s c
stat -c '[%-8s] [%8s] [%08s]' s
mkdir dd; stat -c '%F %a %A' dd
: > empty; stat -c '%F %s' empty
stat -c '%U' s | grep -qx "$(id -un)" && echo "%U is the user"
stat -c '%Y %y' c | sed 's/ [0-9-]* [0-9:.]* [+-][0-9]*$/ LOCAL/'
stat -c '%q|%%' s
stat nothere; echo "status $?"
stat -t s | awk '{print $1, $2, NF}'
stat s | sed -n '1p;2p' | sed 's/IO Block.*//'

echo "=== ls --full-time and --time-style"
touch -d '2021-01-01 00:00:00.25' t
ls --full-time t | awk '{print $6, $7}'
ls -l --time-style=full-iso t | awk '{print $6, $7}'
ls -l --time-style=long-iso t | awk '{print $6, $7}'
ls -l --time-style=+%Y/%m t | awk '{print $6}'
ls -l --time-style=bogus t; echo "status $?"

cd / && rm -rf "$D"
