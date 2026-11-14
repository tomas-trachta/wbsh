declare -u up=hello
declare -l lo=WORLD
echo "$up $lo"
up+=" more"; echo "$up"
