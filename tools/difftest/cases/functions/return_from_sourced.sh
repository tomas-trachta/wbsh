cat > lib.sh <<'EOF'
echo before
return 7
echo after
EOF
source ./lib.sh
echo "rc=$?"
