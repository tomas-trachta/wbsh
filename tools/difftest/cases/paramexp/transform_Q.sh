s="it's a \"test\""
e=''
echo "${s@Q}"
echo "${e@Q}"
a=(x 'y z')
echo "${a[@]@Q}"
