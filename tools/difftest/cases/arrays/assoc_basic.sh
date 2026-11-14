declare -A m=([one]=1 [two]=2)
m[three]=3
echo "${m[one]} ${m[three]} ${#m[@]}"
for k in $(printf '%s\n' "${!m[@]}" | sort); do echo "$k=${m[$k]}"; done
