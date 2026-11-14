fact() { if (( $1 <= 1 )); then echo 1; else echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }
fact 10
fib() { local n=$1; (( n < 2 )) && { r=$n; return; }; fib $((n-1)); local a=$r; fib $((n-2)); r=$((a + r)); }
fib 15; echo $r
