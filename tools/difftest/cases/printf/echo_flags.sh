echo -n "no newline"; echo "|"
echo -e 'a\tb\nc'
echo -E 'a\tb'
echo -ne 'x\cnot printed'; echo
echo -- -n
echo -n -e 'combined\n'
echo -en "x\n"
echo -z
