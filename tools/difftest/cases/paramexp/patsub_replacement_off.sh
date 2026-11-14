# requires: bash>=5.2
shopt -u patsub_replacement
s="hello"
echo "${s/l/[&]}"
