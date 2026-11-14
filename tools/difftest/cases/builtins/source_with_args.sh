cat > lib.sh <<'EOF'
echo "sourced with $# args: $*"
lib_var=set
EOF
set -- outer args
source ./lib.sh one two
echo "after: $* lib_var=$lib_var"
. ./lib.sh
