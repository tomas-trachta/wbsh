f() { echo one; }
f
f() { echo two; }
f
unset -f f
f 2>/dev/null || echo "gone rc=$?"
