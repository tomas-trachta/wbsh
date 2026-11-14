# requires: bash>=5.3
trap 'echo "USR1 trap: $BASH_TRAPSIG"' USR1
trap 'echo "EXIT trap: $BASH_TRAPSIG"' EXIT
kill -USR1 $$
echo "outside: ${BASH_TRAPSIG-unset}"
