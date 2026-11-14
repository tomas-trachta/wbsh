echo "line $LINENO"
f() {
    echo "in f line $LINENO"
}
f
x=$(echo "subst line $LINENO")
echo "$x"
