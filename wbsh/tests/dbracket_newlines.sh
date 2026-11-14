#!/bin/bash
# Newlines may sit between the terms of [[ ... ]] (after [[, &&, ||, (, !
# and before ]] or )), as bash-completion and git-completion write them.

[[ a == a &&
-z "" ]] && echo "and continued"

[[ -n "" ||
   b == b ]] && echo "or continued"

[[
a == a ]] && echo "after [["

[[ a == a
]] && echo "before ]]"

[[ (
   a == a
   )
]] && echo "around parens"

[[ !
   -z "" ]] || echo "after !"

if [[ x == x &&
      y == y ]]; then
    echo "in if"
fi
