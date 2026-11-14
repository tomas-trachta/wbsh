set -- "" '' "$unset_var"
echo "count=$#"
set -- $unset_var ""
echo "count=$#"
