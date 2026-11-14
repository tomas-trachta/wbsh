[ a \< b ] && echo lt
[ b \> a ] && echo gt
[ 010 -eq 10 ] && echo "leading zeros decimal"
[ " 5 " -eq 5 ] 2>/dev/null && echo "spaces ok" || echo "spaces rejected"
