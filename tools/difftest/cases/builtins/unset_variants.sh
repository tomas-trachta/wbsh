a=1; b=2
f() { echo fn; }
unset -v a
unset -f f
unset b nonexistent
echo "[${a-unset}] [${b-unset}] $(type -t f || echo nofunc)"
