( f() { echo defined; }; f )
f 2>/dev/null || echo "not defined outside"
