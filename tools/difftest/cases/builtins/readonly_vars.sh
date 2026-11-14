match() { local re=$1 l; while IFS= read -r l; do [[ $l =~ $re ]] && printf '%s\n' "$l"; done; }
readonly R=1
( R=2 ) 2>&1
echo "rc=$?"
readonly -p | match ' R='
( unset R ) 2>&1
echo "rc=$?"
