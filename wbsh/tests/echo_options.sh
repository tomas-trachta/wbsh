#!/bin/bash
# echo takes only words made entirely of option letters as options.

echo "-n: x"
echo -n: x
echo -nx y
echo -e: 'a\tb'
echo -E: 'a\tb'
echo -
echo --
echo -n -e 'a\tb|'; echo
echo -neE 'c\td|'; echo
echo -nEe 'e\tf|'; echo
echo -n -- x; echo
echo -n "-n" x; echo
echo -e -x 'g\th'
echo x -n
