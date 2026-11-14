f() { echo "in f: \$#=$# \$1=$1"; return 3; }
f x y
echo "\$?=$?"
set -- p q r
echo "\$#=$#"
