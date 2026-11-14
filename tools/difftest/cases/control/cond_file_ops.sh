touch f; mkdir d
[[ -e f && -f f && ! -d f ]] && echo file
[[ -d d ]] && echo dir
[[ -e nope ]] || echo missing
[[ f -nt nope ]] && echo newer-than-missing
