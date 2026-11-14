unset a b
echo "${a:-${b:-inner}}"
b=bval
echo "${a:-${b:-inner}}"
echo "${a:-$(echo cmd)}"
