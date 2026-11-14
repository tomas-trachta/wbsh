s="hello World"
echo "${s^} ${s^^} ${s,} ${s,,} ${s~} ${s~~}"
echo "${s^^[lo]} ${s,,[W]}"
a=(ab cd)
echo "${a[@]^}"
