set -- "a b" c
for x in "$@"; do echo "at:[$x]"; done
for x in "$*"; do echo "star:[$x]"; done
IFS=-
echo "$*"
