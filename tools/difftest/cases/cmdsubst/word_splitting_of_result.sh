set -- $(printf 'a b\tc')
echo $#
set -- "$(printf 'a b\tc')"
echo $#
