[[ "2024-05-17" =~ ^([0-9]{4})-([0-9]{2})-([0-9]{2})$ ]] && echo "${BASH_REMATCH[1]}/${BASH_REMATCH[2]}/${BASH_REMATCH[3]}"
re='a+b'
[[ aaab =~ $re ]] && echo "unquoted var regex: ${BASH_REMATCH[0]}"
[[ 'a+b' =~ "$re" ]] && echo "quoted is literal"
[[ x =~ y ]]; echo "nomatch rc=$? n=${#BASH_REMATCH[@]}"
