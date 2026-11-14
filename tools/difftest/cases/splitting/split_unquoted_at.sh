set -- "a b" "c d"
printf '<%s>' $@; echo
printf '<%s>' $*; echo
