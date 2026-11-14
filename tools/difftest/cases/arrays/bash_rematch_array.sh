[[ "key=value" =~ ^([a-z]+)=([a-z]+)$ ]]
echo "${#BASH_REMATCH[@]} ${BASH_REMATCH[0]} ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
