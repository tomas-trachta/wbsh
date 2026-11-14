set -e
( false; echo "not in subshell" )
echo "not after subshell"
