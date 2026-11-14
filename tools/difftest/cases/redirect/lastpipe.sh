shopt -s lastpipe
set +m
echo value | read -r v
echo "[$v]"
