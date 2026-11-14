FUNCNEST=3
r() { r; }
( r ) 2>&1
echo "rc=$?"
