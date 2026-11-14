[ 1 -lt 2 ] && echo lt
[ abc = abc ] && echo eq
[ abc != abd ] && echo ne
[ -n "" ] || echo empty
[ ! -z x ] && echo not-empty
test 3 -ge 3 && echo ge
[ \( 1 -eq 1 \) -a \( 2 -eq 3 -o 1 -eq 1 \) ] && echo grouped
