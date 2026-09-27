#!/bin/bash
echo "=== brace expansion"
echo {a,b,c}
echo file{1,2,3}.txt
echo {1..5}
echo {1..10..2}
echo {a..e}
echo {01..05}
echo pre-{x,y}-{1,2}-suf
echo "{a,b}"   # quoted, unchanged
echo {a}       # single, unchanged
echo {1..3}{a,b}

echo "=== set -e"
( set -e; true; echo "after true: ok"; false; echo "should NOT print" )
echo "outer continues, status=$?"

echo "=== set -u"
( set -u; x=hi; echo "x=$x"; echo "y=$y" ) 2>&1 || echo "(nounset triggered)"

echo "=== set -x"
( set -x; echo hello; ls -1 FEATURES.md 2>/dev/null ) 2>&1 | head -5

echo "=== set -o pipefail"
( set -o pipefail; false | true | true; echo "rc=$?" )

echo "=== set -f (noglob)"
( set -f; echo *.md )

echo "=== pushd / popd / dirs"
mkdir -p /tmp/wbsh-pd1 /tmp/wbsh-pd2 2>/dev/null
pushd /tmp/wbsh-pd1 > /dev/null
pushd /tmp/wbsh-pd2 > /dev/null
dirs -v
popd > /dev/null
dirs
popd > /dev/null
rm -rf /tmp/wbsh-pd1 /tmp/wbsh-pd2 2>/dev/null

echo "=== time"
time sleep 0.05 2>&1 | tail -1

echo "=== \$- with options"
( set -eu; echo "\$- = $-" )

echo "=== done"
