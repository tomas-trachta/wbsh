#!/bin/bash
# ar writing: r / q / d / m with a / b positioning, GNU long names in the
# // table, deterministic headers (and U for real dates), the old-style key
# and the dash form, and BSD #1/ names read back and rewritten.

D=_arw_tmp
rm -rf $D 2>/dev/null
mkdir -p $D
cd $D || exit 1
printf 'alpha\n' > a.o
printf 'bravo!\n' > b.o
printf 'charlie\n' > c.o
printf 'delta\n' > d.o
printf 'long member\n' > a_very_long_member_name.o
printf 'second long\n' > another_long_object.o

echo "=== create"
ar rc lib.a a.o b.o
ar t lib.a
cat lib.a

echo "=== creating message, quick append"
ar r new.a a.o 2>&1
ar q new.a b.o a.o
ar t new.a

echo "=== long names"
ar rcs long.a a.o a_very_long_member_name.o another_long_object.o
cat long.a
echo
ar t long.a
ar p long.a another_long_object.o

echo "=== replace"
printf 'ALPHA\n' > a.o
ar rv lib.a a.o c.o
ar p lib.a a.o
ar pv lib.a c.o

echo "=== positioning"
ar mb a.o lib.a c.o
ar t lib.a | tr '\n' ' '; echo
ar mav c.o lib.a b.o
ar t lib.a | tr '\n' ' '; echo
ar m lib.a c.o
ar t lib.a | tr '\n' ' '; echo
ar rb b.o lib.a d.o
ar t lib.a | tr '\n' ' '; echo
ar rav c.o lib.a b.o
ar t lib.a | tr '\n' ' '; echo

echo "=== delete"
ar dv lib.a b.o missing.o
ar t lib.a | tr '\n' ' '; echo

echo "=== dash form"
ar -rcs dash.a d.o c.o
ar -tv dash.a | awk '{ print $1, $2, $3, $NF }'
ar -r -c dash2.a a.o && ar -t dash2.a

echo "=== extract"
mkdir x
cd x
ar x ../long.a a_very_long_member_name.o
cat a_very_long_member_name.o
ar xv ../lib.a
ls
cd ..

echo "=== real dates"
touch -d @981173106 c.o
ar rcU dated.a c.o
head -c 68 dated.a | tail -c 60
ar tv dated.a | awk '{ print $1, $2, $3, $NF }'
ar ruv dated.a c.o
echo "unchanged older"
touch -d @981173200 c.o
ar ruvU dated.a c.o
mkdir y
cd y
ar xo ../dated.a c.o
stat -c %Y c.o
cd ..

echo "=== BSD long names"
{
    printf '!<arch>\n'
    printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' '#1/20' 0 0 0 644 25
    printf 'bsd_long_name_here.oBSD!\n\n'
    printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' 'short.o/' 0 0 0 644 4
    printf 'abc\n'
} > bsd.a
ar t bsd.a
ar p bsd.a bsd_long_name_here.o
ar q bsd.a a.o
cat bsd.a

echo "=== errors"
ar t nothere.a; echo "status $?"
ar d nothere.a a.o; echo "status $?"
ar rc lib.a nothere.o; echo "status $?"
ar t a.o; echo "status $?"
ar m lib.a nothere.o; echo "status $?"
ar x lib.a nothere.o; echo "status $?"
ar z lib.a; echo "status $?"
ar rt lib.a; echo "status $?"
ar t lib.a | tr '\n' ' '; echo

cd ..
rm -rf $D
echo "=== done"
