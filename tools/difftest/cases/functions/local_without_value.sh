x=outer
f() { local x; echo "[${x-unset}]"; x=set; }
f; echo "$x"
