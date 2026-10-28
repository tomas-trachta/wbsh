#!/bin/bash
# The ulimit builtin.

echo "=== -a"
ulimit -a | sed -e 's/[0-9][0-9]*$/N/'
echo "=== single limits"
ulimit
ulimit -f
n=$(ulimit -n)
(( n >= 20 )) && echo "open files is a number"
ulimit -Hn
s=$(ulimit -s)
(( s > 0 )) && echo "stack size is a number"
ulimit -p
ulimit -c -d | sed -e 's/[0-9][0-9]*$/N/'

echo "=== setting"
ulimit -c 0; echo "rc=$? core $(ulimit -c) hard $(ulimit -Hc)"
ulimit -Sc 5; echo "rc=$? core $(ulimit -c)"
ulimit -Hv 1000; echo "rc=$? (soft limit is still unlimited)"
ulimit -Sv 500; ulimit -Hv 1000; echo "rc=$? hard v $(ulimit -Hv) soft v $(ulimit -Sv)"
ulimit -v 2000; echo "rc=$?"
ulimit -Hv unlimited; echo "rc=$?"
ulimit -n 1000; echo "rc=$? open files $(ulimit -n)"
ulimit -n hard; echo "rc=$? open files $(ulimit -n)"
ulimit -n 99999; echo "rc=$?"
ulimit -p 4; echo "rc=$?"

echo "=== errors"
ulimit -z; echo "rc=$?"
ulimit -c abc; echo "rc=$?"
