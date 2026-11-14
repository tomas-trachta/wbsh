declare -A m=([k]=v [e]=)
[[ -v m[k] ]] && echo has-k
[[ -v m[e] ]] && echo has-e
[[ -v m[z] ]] || echo no-z
unset 'm[k]'
echo "${#m[@]}"
