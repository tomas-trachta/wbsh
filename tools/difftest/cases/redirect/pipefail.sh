set -o pipefail
false | true; echo "rc=$?"
true | (exit 3) | true; echo "rc=$?"
set +o pipefail
false | true; echo "rc=$?"
