#!/bin/bash
# Tilde expansion in the word of ${name:-word} and friends, and in words
# that only look like assignments, which POSIX mode leaves alone.

HOME=/usr/xyz
XPATH=/bin:/usr/bin:.

unset ADDPATH
: ${ADDPATH:=~/bin:~/bin2:$XPATH}
echo $ADDPATH
echo ${TPATH:-~}
echo "${TPATH:-~}"
echo ${TPATH:-"~"}
echo ${TPATH:-\~/x}
echo ${XPATH:+~/y}
echo ${TPATH:-~/a:~/b}

echo foo=bar:~
set -o posix
echo foo=bar:~
h=HOME=~
eval echo $h
foo=~:~
echo $foo
set +o posix
eval echo $h
