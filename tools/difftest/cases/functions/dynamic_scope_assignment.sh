f() { local v=f; g; echo "f: $v"; }
g() { v=changed-by-g; }
v=global
f
echo "global: $v"
