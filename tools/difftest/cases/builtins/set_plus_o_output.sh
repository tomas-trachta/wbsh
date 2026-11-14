match() { local re=$1 l; while IFS= read -r l; do [[ $l =~ $re ]] && printf '%s\n' "$l"; done; }
set -o noglob
set +o | match 'noglob|errexit'
