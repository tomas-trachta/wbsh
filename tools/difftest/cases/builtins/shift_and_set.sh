set -- a b c d e
shift
echo "$*"
shift 2
echo "$* ($#)"
shift 5
echo "rc=$? $*"
set --
echo "$#"
