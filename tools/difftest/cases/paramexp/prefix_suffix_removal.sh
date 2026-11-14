p=/usr/local/lib/libfoo.so.1.2
echo "${p#*/} | ${p##*/} | ${p%.*} | ${p%%.*}"
echo "${p#nomatch} | ${p%/*}"
