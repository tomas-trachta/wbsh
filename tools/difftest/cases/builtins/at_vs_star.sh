set -- "a  b" "" c
printf '<%s>' "$@"; echo
printf '<%s>' "$*"; echo
printf '<%s>' $@; echo
printf '<%s>' $*; echo
printf '<%s>' "${@}x"; echo
printf '<%s>' "x$@y"; echo
