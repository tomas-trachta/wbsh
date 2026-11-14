# requires: bash>=5.3
mkdir g && cd g
printf 'aaaa' > b; printf 'a' > c; printf 'aa' > a
mkdir 10 9 100
GLOBSORT=size; echo [abc]
GLOBSORT=-size; echo [abc]
GLOBSORT=-name; echo *
GLOBSORT=numeric; echo *
GLOBSORT=-numeric; echo *
GLOBSORT=+bogus; echo *
GLOBSORT=-; echo *
unset GLOBSORT; echo *
