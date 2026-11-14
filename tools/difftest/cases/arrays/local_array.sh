f() { local -a arr=(x y); arr+=(z); echo "${arr[@]}"; }
f
echo "[${arr[@]}]"
