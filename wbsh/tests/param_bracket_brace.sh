#!/bin/bash
# A `{` in a bracket expression of a ${...} pattern is a character to match,
# as in bash-completion's `${o%%[<{().[]*}`; it opens no brace.

o='ab<cd'; echo "1 ${o%%[<{().[]*}"
o='a{b'; echo "2 ${o%%[{]*}"
o='x[y{z'; echo "3 ${o#*[[]}"
x=; echo "4 ${x:-[}"
o='a]b{c'; echo "5 ${o%%[]{]*}"
o='a{b'; echo "6 ${o/[!a]/-}"
echo "7 ${o%%[{]*}" | tr -d ' '
