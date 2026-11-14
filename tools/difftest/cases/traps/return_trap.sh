trap 'echo "RETURN from ${FUNCNAME[0]}"' RETURN
f() { echo in f; }
f
set -o functrace
g() { echo in g; }
g
