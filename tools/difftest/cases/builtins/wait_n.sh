# requires: bash>=4.3
(exit 4) &
wait -n
echo "rc=$?"
