hex() { local s=$1 i; for ((i=0; i<${#s}; i++)); do printf '%02x ' "'${s:i:1}"; done; echo "(${#s})"; }
hex $'tab:\there'
hex $'nl:\\n'
echo $'hex:\x41\x42' $'oct:\101' $'u:\u0041' $'quote:\''
hex $'esc-e:\e'
