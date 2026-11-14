printf '<%s>' $(printf 'x y\nz\n'); echo
printf '<%s>' "$(printf 'x y\nz\n\n\n')"; echo
