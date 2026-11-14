trap 'echo parent exit' EXIT
( echo child )
trap - EXIT
