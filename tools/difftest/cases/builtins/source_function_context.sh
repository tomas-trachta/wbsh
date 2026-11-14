cat > lib.sh <<'EOF'
where() { echo "${FUNCNAME[0]} from ${BASH_SOURCE[0]} called at line ${BASH_LINENO[0]}"; }
echo "top of lib: ${BASH_SOURCE[0]} <- ${BASH_SOURCE[1]}"
echo "lib FUNCNAME: ${FUNCNAME[*]}"
EOF
source ./lib.sh
where
f() { source ./lib.sh; }
f
