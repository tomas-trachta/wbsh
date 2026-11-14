match() { local re=$1 l; while IFS= read -r l; do [[ $l =~ $re ]] && printf '%s\n' "$l"; done; }
export MY_EXPORT_A=1 MY_EXPORT_B='two words'
export -n MY_EXPORT_B
export -p | match MY_EXPORT
declare -p MY_EXPORT_B
