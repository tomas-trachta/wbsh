for i in a b; do echo $i; done
set -- p q
for i; do echo "pos $i"; done
for i in; do echo never; done
echo "empty list ok"
