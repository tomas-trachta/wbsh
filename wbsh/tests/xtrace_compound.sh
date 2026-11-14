#!/bin/bash
# set -x traces assignments, for / select / case headers and (( )), and
# BASH_XTRACEFD sends the trace to another descriptor.

exec 2>&1
set -x
for ((i=0; i<2; i++ )); do
    x=0
done
for ((;;)); do break; done
for i in a "b c"; do :; done
case "$x y" in
    *) y=1 ;;
esac
foo=one
foo+=two
arr=(a $'\n' "b c"$'d' [5]=e)
arr[1+1]=q
z= w='a b'
(( foo2 = 1 + 2 ))
set +x

f=$(mktemp)
BASH_XTRACEFD=7
exec 4>"$f"
BASH_XTRACEFD=4
set -x
echo traced
unset BASH_XTRACEFD
exec 4>&-
set +x
echo "trace file:"
cat "$f"
rm -f "$f"
