outer() { inner; }
inner() { echo "${FUNCNAME[@]}"; echo "${#FUNCNAME[@]}"; }
outer
echo "top: [${FUNCNAME[*]}]"
