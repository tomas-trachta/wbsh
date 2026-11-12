#!/bin/bash
# The replacement of ${var/pattern/replacement}: bash 5.2 removes
# backslashes and, with patsub_replacement (on by default), puts the
# matched text in place of an unquoted &.
x=foo
echo "1 ${x/o/\z}"
echo "2 ${x/o/a\/b}"
echo "3 ${x/o/a\b}"
echo 4 ${x/o/\z}
echo "5 ${x/o/'\z'}"
echo "6 ${x/o/\&}"
echo "7 ${x/o/[&]}"
echo "8 ${x//o/<&>}"
shopt -u patsub_replacement 2>/dev/null
echo "9 ${x/o/[&]}"
