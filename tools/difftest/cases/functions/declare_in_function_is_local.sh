f() { declare d=inner; declare -g g=global; }
f
echo "[${d-unset}] [$g]"
