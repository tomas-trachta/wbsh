[[ 10 -gt 9 && ! 1 -eq 2 ]] && echo ok1
[[ 1+1 -eq 2 ]] && echo arith-in-operands
x=5
[[ x -eq 5 ]] && echo var-name-in-arith
[[ ( 1 -lt 2 ) || 3 -lt 1 ]] && echo grouped
