printf 'x,y,z' | { mapfile -t -d , parts; printf '[%s]' "${parts[@]}"; echo; }
