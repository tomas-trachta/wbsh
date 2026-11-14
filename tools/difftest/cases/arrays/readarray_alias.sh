readarray -t r <<< $'1\n2'
echo "${r[1]}"
