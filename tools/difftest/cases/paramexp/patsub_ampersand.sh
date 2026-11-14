# requires: bash>=5.2
s="hello world"
echo "${s/o/[&]}"
echo "${s//o/<&&>}"
echo "${s/o/\&}"
r='&'
echo "${s/o/$r}" "${s/o/"&"}"
