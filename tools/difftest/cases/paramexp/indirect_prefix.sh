myA=1 myB=2 other=3
echo "${!my*}"
for v in "${!my@}"; do echo "$v"; done
