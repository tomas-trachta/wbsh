#!/bin/bash
# In [[ ]], an unquoted unary operator (-a .. -z, -G -L -N -O -R -S)
# always takes the next word as its operand, as in bash: `-x == y` tests
# a file named `==` and then is a syntax error, and `-n` alone is an
# error, not a test of the string "-n". (bash's messages differ a bit.)

try() {
    eval "[[ $1 ]]" 2>/dev/null
    echo "[[ $1 ]] status $?"
}

try '-x == y'
try '-n == y'
try '! -x == y'
try '-x -a y'
try '-n'
try '-x'
try '-x && y'
try '-d / || -n'
try '-x =='
try 'a == -x'
try '-o x'
try '"-n"'
try '-n x && -z ""'
try '-e /'
