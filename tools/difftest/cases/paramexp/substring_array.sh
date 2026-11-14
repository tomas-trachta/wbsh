a=(zero one two three four)
echo "${a[@]:1:2}" "|" "${a[@]: -2}" "|" "${a[1]:1:2}"
