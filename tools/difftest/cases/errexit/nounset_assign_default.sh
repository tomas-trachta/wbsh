set -u
echo "${v:=assigned}"
echo "$v"
: "${w:+x}"
echo ok
