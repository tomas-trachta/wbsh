a=(x.c y.c z.h)
echo "${a[@]%.c}" "|" "${a[@]#?.}"
set -- one.txt two.txt
echo "${@%.txt}"
