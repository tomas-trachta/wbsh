printf 'line1\nline2\n' > in
while read -r l; do echo "got $l"; done < in
read -r first < in; echo "$first"
