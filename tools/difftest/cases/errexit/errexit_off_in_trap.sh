set -e
trap 'false; echo "in trap after false"' EXIT
echo body
