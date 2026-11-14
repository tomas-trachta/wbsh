args=(one 'two words' "th'ree")
printf -v cmd '%q ' "${args[@]}"
eval "set -- $cmd"
echo "$# [$2] [$3]"
