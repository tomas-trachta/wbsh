hex() { local s=$1 i; for ((i=0; i<${#s}; i++)); do printf '%02x ' "'${s:i:1}"; done; echo "(${#s})"; }
hex $'\cA'
hex $'a\0b'
hex $'\x7'
hex $'\x41g'
