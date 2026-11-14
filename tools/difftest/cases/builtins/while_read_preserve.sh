printf '  a  \n\tb\\\n' | while IFS= read -r line; do printf '[%s]\n' "$line"; done
