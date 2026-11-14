# requires: bash>=5.1
declare -A m=([one]=1)
a=(x 'y z')
echo "${m[@]@K}"
echo "${a[@]@K}"
