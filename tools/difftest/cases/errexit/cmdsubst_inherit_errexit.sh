# requires: bash>=4.4
set -e
shopt -s inherit_errexit
x=$(false; echo "should not print")
echo "x=[$x]"
