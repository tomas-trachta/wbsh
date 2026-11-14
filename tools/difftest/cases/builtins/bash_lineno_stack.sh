a() { b; }
b() { echo "${BASH_LINENO[@]}"; echo "${FUNCNAME[@]}"; }
a
