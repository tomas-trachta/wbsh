f() { echo to-out; echo to-err >&2; }
{ f 3>&1 1>&2 2>&3 3>&-; } 2>err.txt 1>out.txt
echo "out: $(cat out.txt)"
echo "err: $(cat err.txt)"
