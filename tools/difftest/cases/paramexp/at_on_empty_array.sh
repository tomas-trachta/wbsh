a=()
set -- "${a[@]}"
echo "$#"
set -- "${a[@]:-def}"
echo "$# $1"
