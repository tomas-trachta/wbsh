x=global
f() { local x=local; g; }
g() { echo "g sees $x"; }
f
g
