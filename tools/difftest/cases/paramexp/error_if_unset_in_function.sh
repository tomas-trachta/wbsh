f() { echo "${1:?need an argument}"; echo not reached; }
( f ) 2>&1
echo "rc=$?"
f ok
