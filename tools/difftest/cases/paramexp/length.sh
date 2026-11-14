s="hello world"; a=(one two three)
echo "${#s} ${#a} ${#a[@]} ${#a[1]} ${#}"
set -- a b c d
echo "${#} ${#@} ${#*}"
