a=("a b" "c" "")
printf '<%s>' "${a[@]}"; echo
printf '<%s>' ${a[@]}; echo
printf '<%s>' "${a[*]}"; echo
IFS=,; printf '<%s>' "${a[*]}"; echo
