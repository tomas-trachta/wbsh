declare -A m=([k]=a)
m[k]+=b
m[n]+=new
echo "${m[k]} ${m[n]}"
