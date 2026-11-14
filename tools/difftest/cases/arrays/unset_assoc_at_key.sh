# requires: bash>=5.2
declare -A m
k='@'
m[$k]=at
m[x]=1
unset 'm[@]'
declare -p m
