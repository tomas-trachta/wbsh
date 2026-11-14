s=$'tab\there "q" \\back'
printf -v q '%q' "$s"
eval "r=$q"
[ "$r" = "$s" ] && echo roundtrip-ok
