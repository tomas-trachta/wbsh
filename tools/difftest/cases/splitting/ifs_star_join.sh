set -- a b c
IFS=:; echo "$*"
IFS=; echo "$*"
unset IFS; echo "$*"
