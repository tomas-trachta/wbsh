trap '' USR1
kill -USR1 $$
echo survived
trap -p USR1
