a=(1 2 3 4)
echo "${a[-1]} ${a[-2]}"
a[-1]=last
echo "${a[@]}"
unset 'a[-2]'
echo "${a[@]} ${!a[@]}"
