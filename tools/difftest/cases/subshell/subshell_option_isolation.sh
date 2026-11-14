( set -f; case $- in *f*) echo "noglob in sub" ;; esac )
case $- in *f*) echo leaked ;; *) echo "not leaked" ;; esac
