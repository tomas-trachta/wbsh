x=outer
f() { local x=inner; unset x; echo "[${x-unset}]"; }
f; echo "$x"
