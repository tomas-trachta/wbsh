x=world
cat <<EOF
hello $x
sum $((1+2)) $(echo cmd)
escaped \$x \`q\` back\\slash
EOF
