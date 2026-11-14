#!/bin/bash
# getopts: OPTERR=0 silences its messages, an option before the optstring
# is an invalid option, and the usage line reads as bash 5.3's.

exec 2>&1
getopts
echo "status $?"
getopts -a opts name
echo "status $?"

set -- -a -c -b
OPTERR=0
while getopts ab opt; do echo "got $opt"; done
OPTIND=1
OPTERR=1
while getopts ab opt; do echo "got $opt"; done

declare -lu both=AbC
declare -p both
declare -u up=abc
declare -l up
up=XyZ
declare -p up

set -- -a
OPTIND=1
getopts :ab: opt-var "$@"
echo "status $? OPTIND=$OPTIND"

declare -r RO=foo
declare -n OPTARG=RO
set -- -z
OPTIND=1
getopts x x
declare -p RO
