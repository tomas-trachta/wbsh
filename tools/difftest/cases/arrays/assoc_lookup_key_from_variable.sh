declare -A m=([a]=1 [b]=2)
m['*']=lit
k='*'
echo "[${m[$k]}]"
k='@'
echo "[${m[$k]-unset}]"
echo "${#m[@]}"
