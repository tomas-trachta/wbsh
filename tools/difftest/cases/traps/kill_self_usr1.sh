trap 'echo caught USR1' USR1
kill -USR1 $$
echo continued
