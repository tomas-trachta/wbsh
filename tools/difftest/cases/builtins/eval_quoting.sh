v='a b'
eval "set -- $v"; echo $#
eval "set -- \"$v\""; echo $#
eval 'arr=(' "'1 2'" 3 ')'; echo "${#arr[@]}"
