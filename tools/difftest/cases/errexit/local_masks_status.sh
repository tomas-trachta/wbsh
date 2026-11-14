set -e
f() { local x=$(false); echo "local masked rc"; }
f
