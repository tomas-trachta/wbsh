x=$(exit 3)
echo "rc=$?"
x=$(true)$(false)
echo "rc=$?"
