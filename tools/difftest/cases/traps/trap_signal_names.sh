trap 'echo usr1' USR1
trap 'echo term' SIGTERM
trap -p USR1 TERM
