f() { caller 0 | { read -r line func file; echo "$line $func ${file##*/}"; }; }
g() { f; }
g
