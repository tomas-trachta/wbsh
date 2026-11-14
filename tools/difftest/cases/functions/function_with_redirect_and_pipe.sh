f() { while read -r l; do echo "f:$l"; done; }
printf 'x\ny\n' | f
