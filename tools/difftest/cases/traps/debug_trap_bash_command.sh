trap 'case $BASH_COMMAND in echo*) printf "about to: %s\n" "$BASH_COMMAND" ;; esac' DEBUG
echo one
x=2
echo two
