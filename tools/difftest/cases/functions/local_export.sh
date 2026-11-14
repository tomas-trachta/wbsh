f() { local -x LV=exported; declare -p LV; }
f
echo "[${LV-unset}]"
