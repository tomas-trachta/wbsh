# requires: bash>=5.1
s="hello World"
echo "${s@U} ${s@L} ${s@u}"
a=(ab cd)
echo "${a[@]@U}"
