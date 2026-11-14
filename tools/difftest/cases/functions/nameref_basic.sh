set_it() { local -n ref=$1; ref="set via nameref"; }
set_it target
echo "$target"
