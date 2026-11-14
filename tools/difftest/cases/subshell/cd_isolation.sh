mkdir sub
here=$PWD
( cd sub && echo "${PWD##*/}" )
[ "$PWD" = "$here" ] && echo "same dir"
