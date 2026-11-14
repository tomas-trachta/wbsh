declare -A m
m["a b"]=space
m[*]=star
m[@]=at
m[-1]=neg
k='$x'; m[$k]=dollar
for k in "a b" '*' '@' -1 '$x'; do echo "[$k]=${m[$k]}"; done
