# requires: bash>=5.3
a=$BASH_MONOSECONDS
b=$BASH_MONOSECONDS
(( a > 0 && b >= a )) && echo monotonic
