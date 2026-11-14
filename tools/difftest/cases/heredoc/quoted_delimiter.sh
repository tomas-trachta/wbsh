x=world
cat <<'EOF'
hello $x $(echo no) \n
EOF
cat <<"E2"
also $x
E2
cat <<E\3
partial $x
E3
