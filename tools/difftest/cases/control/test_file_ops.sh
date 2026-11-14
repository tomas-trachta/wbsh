touch f
mkdir d
[ -f f ] && echo f
[ -d d ] && echo d
[ -s f ] || echo "f empty"
echo x > f
[ -s f ] && echo "f non-empty"
[ -e d/nothing ] || echo none
[ f -ot nope ] || echo "ot missing false"
