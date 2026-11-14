[[ ${BASH_VERSINFO[0]} =~ ^[0-9]+$ ]] && echo major-numeric
echo "${#BASH_VERSINFO[@]}"
