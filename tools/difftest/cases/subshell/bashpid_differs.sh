p=$BASHPID
( [ "$BASHPID" != "$p" ] && echo "subshell has own BASHPID" )
[ "$BASHPID" = "$p" ] && echo "same in main"
( [ "$$" = "$(echo $$)" ] && echo "\$\$ is inherited" )
