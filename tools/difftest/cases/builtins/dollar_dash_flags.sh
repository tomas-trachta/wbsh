case $- in *h*) echo has-h ;; esac
case $- in *B*) echo has-B ;; esac
case $- in *i*) echo interactive ;; *) echo non-interactive ;; esac
set -u
case $- in *u*) echo has-u ;; esac
