for i in 1 2; do false; done
echo "rc=$?"
for i in; do false; done
echo "rc=$?"
while false; do :; done
echo "rc=$?"
