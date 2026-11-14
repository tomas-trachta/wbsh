unset u
set -- ${u:-a b c}
echo $#
set -- "${u:-a b c}"
echo $#
