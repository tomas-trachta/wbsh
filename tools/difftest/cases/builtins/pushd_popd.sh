mkdir -p a/b
pushd a >/dev/null
pushd b >/dev/null
echo "${PWD##*/} depth=${#DIRSTACK[@]}"
popd >/dev/null
echo "${PWD##*/}"
popd >/dev/null
echo "depth=${#DIRSTACK[@]}"
