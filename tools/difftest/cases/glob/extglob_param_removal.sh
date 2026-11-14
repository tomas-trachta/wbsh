shopt -s extglob
s="   padded   "
echo "[${s##+([[:space:]])}]"
echo "[${s%%+([[:space:]])}]"
v=aaabbb
echo "${v//+(a)/X}"
