mapfile -t lines <<'EOF'
first
second line

fourth
EOF
echo "${#lines[@]}"
printf '[%s]\n' "${lines[@]}"
