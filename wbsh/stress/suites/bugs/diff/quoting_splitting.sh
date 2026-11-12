#!/bin/bash
# Word splitting, quoting and IFS.
show() { printf '<%s>' "$@"; echo; }
v='  a  b	c  '
show $v
show "$v"
IFS=: ; x='a::b:'; show $x; unset IFS
IFS=', ' ; x='a, b,,c ,'; show $x; unset IFS
show "$@" "${@}" "$*"
set -- 'one two' three ''
show "$@"
show $@
show "$*"
IFS=-; show "$*"; unset IFS
show '' "" $'a\tb' $'\x41é' "a\$b" 'a\nb'
e=
show $e "$e" ${e:-} "${e:-}"
show a${e}b "a${e}b"
