#!/bin/bash
# A shell variable whose name differs from an exported one only in case
# (`tmp` beside TMP) must not shadow it in a pipeline stage: Windows looks
# names up regardless of case, and the stage makes its temporary files
# where the process TMP points.

echo "=== 1. unexported tmp, temp and tmpdir"
tmp=/c/wbsh-no-such-dir
temp=/c/wbsh-no-such-dir
tmpdir=/c/wbsh-no-such-dir
echo x | { echo "[$(echo hi)]"; }
echo x | awk '{ print "awk pipe" | "cat" }'
echo x | { echo "$tmp $temp $tmpdir"; }
echo x | { env | grep -c '^tmp='; }

echo "=== 2. exported as well"
export tmp
echo x | { echo "[$(echo hi)]"; }
echo x | awk '{ print "awk pipe" | "cat" }'
echo x | { env | grep -c '^tmp='; }
unset tmp temp tmpdir
