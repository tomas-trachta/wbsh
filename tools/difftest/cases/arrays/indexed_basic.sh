a=(one two three)
echo "${a[0]} ${a[2]} ${a} ${#a[@]}"
a[5]=six
echo "${#a[@]} ${!a[*]}"
