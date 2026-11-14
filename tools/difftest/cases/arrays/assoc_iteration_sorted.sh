declare -A colors=([red]=ff0000 [green]=00ff00 [blue]=0000ff)
for k in "${!colors[@]}"; do echo "$k ${colors[$k]}"; done | sort
