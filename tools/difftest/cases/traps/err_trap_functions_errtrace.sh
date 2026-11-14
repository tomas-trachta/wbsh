trap 'echo "ERR in ${FUNCNAME[0]:-main}"' ERR
f() { false; }
f
set -E
f
