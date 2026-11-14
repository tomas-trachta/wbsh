# requires: bash>=4.4
f() { local -; set -u; case $- in *u*) echo "inside: u set" ;; esac; }
f
case $- in *u*) echo "u leaked" ;; *) echo "u restored" ;; esac
