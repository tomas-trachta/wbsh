RANDOM=42
a=$RANDOM
RANDOM=42
b=$RANDOM
[ "$a" = "$b" ] && echo reproducible
