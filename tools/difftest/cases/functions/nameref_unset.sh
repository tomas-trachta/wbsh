declare -n r=target
target=x
unset r
echo "[${target-unset}]"
unset -n r
echo "[${r-unset}]"
