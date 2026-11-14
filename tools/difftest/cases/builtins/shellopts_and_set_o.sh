match() { local re=$1 l; while IFS= read -r l; do [[ $l =~ $re ]] && printf '%s\n' "$l"; done; }
set -o | match '^(errexit|nounset|pipefail|noglob)[[:space:]]'
set -eu -o pipefail
set -o | match '^(errexit|nounset|pipefail|noglob)[[:space:]]'
set +eu +o pipefail
