# requires: bash>=5.2
a=(x 'y z')
printf '<%s>' "${a[@]@k}"; echo
