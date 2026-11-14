true &
wait $!
echo "rc=$?"
(exit 3) &
wait $!
echo "rc=$?"
